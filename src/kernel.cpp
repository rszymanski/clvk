// Copyright 2018 The clvk authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>

#include "memory.hpp"
#include "kernel.hpp"

VkPipeline cvk_kernel_pipeline_cache::get_pipeline(uint32_t x, uint32_t y, uint32_t z) {
    VkPipeline ret = VK_NULL_HANDLE;

    m_lock.lock();
    for (auto &entry : m_entries) {
        if ((entry.lws[0] == x) && (entry.lws[1] == y) && (entry.lws[2] == z)) {
            ret = entry.pipeline;
            break;
        }
    }

    if (ret == VK_NULL_HANDLE) {
        ret = create_and_insert_pipeline(x, y, z);
    }
    m_lock.unlock();
    return ret;
}

VkPipeline cvk_kernel_pipeline_cache::create_and_insert_pipeline(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t lws[3] = {x,y,z};

    VkSpecializationMapEntry mapEntries[3] = {
        {0, 0 * sizeof(uint32_t), sizeof(uint32_t)},
        {1, 1 * sizeof(uint32_t), sizeof(uint32_t)},
        {2, 2 * sizeof(uint32_t), sizeof(uint32_t)},
    };

    VkSpecializationInfo specialiaztionInfo = {
        3,
        mapEntries,
        sizeof(lws),
        &lws,
    };

    const VkComputePipelineCreateInfo createInfo = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, // sType
        nullptr, // pNext
        0, // flags
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, // sType
            nullptr, // pNext
            0, // flags
            VK_SHADER_STAGE_COMPUTE_BIT, // stage
            m_kernel->program()->shader_module(), // module
            m_kernel->name().c_str(),
            &specialiaztionInfo // pSpecializationInfo
        }, // stage
        m_kernel->pipeline_layout(), // layout
        VK_NULL_HANDLE, // basePipelineHandle
        0 // basePipelineIndex
    };

    VkPipeline pipeline;
    auto vkdev = m_device->vulkan_device();
    VkResult res = vkCreateComputePipelines(vkdev, VK_NULL_HANDLE, 1, &createInfo, nullptr, &pipeline);

    if (res != VK_SUCCESS) {
        cvk_error_fn("Could not create compute pipeline: %s", vulkan_error_string(res));
        return VK_NULL_HANDLE;
    }

    insert_pipeline(x, y, z, pipeline);

    return pipeline;
}

void cvk_kernel::build_descriptor_sets_layout_bindings()
{
    bool pod_found = false;

    for (auto &arg : m_args) {
        VkDescriptorType dt = VK_DESCRIPTOR_TYPE_MAX_ENUM;

        switch (arg.kind) {
        case kernel_argument_kind::buffer:
            dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            break;
        case kernel_argument_kind::ro_image:
            dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            break;
        case kernel_argument_kind::wo_image:
            dt = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            break;
        case kernel_argument_kind::sampler:
            dt = VK_DESCRIPTOR_TYPE_SAMPLER;
            break;
        case kernel_argument_kind::local:
            continue;
        case kernel_argument_kind::pod:
        case kernel_argument_kind::pod_ubo:
            if (!pod_found) {
                if (arg.kind == kernel_argument_kind::pod) {
                    dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                } else if (arg.kind == kernel_argument_kind::pod_ubo) {
                    dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                }

                m_pod_descriptor_type = dt;

                pod_found = true;
            } else {
                continue;
            }
        }

        VkDescriptorSetLayoutBinding binding = {
            arg.binding, // binding
            dt, // descriptorType
            1, // decriptorCount
            VK_SHADER_STAGE_COMPUTE_BIT, // stageFlags
            nullptr // pImmutableSamplers
        };

        m_layout_bindings.push_back(binding);
    }
}

bool cvk_kernel::allocate_pod_buffer()
{
    cl_int err;
    auto mem = cvk_mem::create(m_context, 0, pod_size(), nullptr, &err);
    if (err != CL_SUCCESS) {
        return false;
    }

    m_pod_buffer = std::move(mem);
    return true;
}

cl_int cvk_kernel::init()
{
    // Get a pointer to the arguments from the program
    auto args = m_program->args_for_kernel(m_name);

    if (args == nullptr) {
        cvk_error("Kernel %s doesn't exist in program", m_name.c_str());
        return CL_INVALID_KERNEL_NAME;
    }

    // Store a sorted copy of the arguments
    m_args = *args;
    std::sort(m_args.begin(), m_args.end(), [](kernel_argument a, kernel_argument b) {
        return a.pos < b.pos;
    });

    // Create Descriptor Sets Layout Bindings
    build_descriptor_sets_layout_bindings();

    // Create Descriptor Sets Layout
    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        0,
        0,
        static_cast<uint32_t>(m_layout_bindings.size()),
        m_layout_bindings.data()
    };

    auto vkdev = m_context->device()->vulkan_device();

    VkResult res = vkCreateDescriptorSetLayout(vkdev, &descriptorSetLayoutCreateInfo, 0, &m_descriptor_set_layout);

    if (res != VK_SUCCESS) {
        cvk_error("Could not create descriptor set layout");
        return CL_INVALID_VALUE;
    }

    // Create argument storage holders // TODO all the storage for PODs is wasted
    m_args_storage.resize(m_args.size());

    for (uint32_t i = 0; i < m_args.size(); ++i) {
        auto const &arg = m_args[i];

        cvk_kernel_arg_storage *storage = &m_args_storage[arg.pos];

        if (arg.kind == kernel_argument_kind::buffer) {
            storage->ptr = new char[sizeof(cvk_mem*)];
            storage->size = sizeof(cvk_mem*);
        }
    }

    // Init POD arguments
    if (has_pod_arguments()) {

        // Find out POD binding
        for (auto &arg : m_args) {
            if (arg.is_pod()) {
                m_pod_binding = arg.binding;
                break;
            }
        }

        if (m_pod_binding == INVALID_POD_BINDING) {
            return CL_INVALID_PROGRAM;
        }

        // Check we know the POD buffer's descriptor type
        if (m_pod_descriptor_type == VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            return CL_INVALID_PROGRAM;
        }

        // Create initial POD buffer
        if (!allocate_pod_buffer()) {
            return CL_OUT_OF_RESOURCES;
        }
    }

    // Create pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        0,
        0,
        1,
        &m_descriptor_set_layout,
        0,
        0
    };

    res = vkCreatePipelineLayout(vkdev, &pipelineLayoutCreateInfo, 0, &m_pipeline_layout);
    if (res != VK_SUCCESS) {
        cvk_error("Could not create pipeline layout.");
        return CL_INVALID_VALUE;
    }

    // Determine number and types of bindings
    std::unordered_map<VkDescriptorType, uint32_t> bindingTypes;
    for (auto &lb : m_layout_bindings) {
        bindingTypes[lb.descriptorType]++;
    }

    std::vector<VkDescriptorPoolSize> poolSizes(bindingTypes.size());

    int bidx = 0;
    for (auto &bt : bindingTypes) {
        poolSizes[bidx].type = bt.first;
        poolSizes[bidx].descriptorCount = bt.second;
        bidx++;
    }

    // Create descriptor pool
    VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        nullptr,
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, // flags
        cvk_kernel::MAX_INSTANCES, // maxSets
        static_cast<uint32_t>(poolSizes.size()), // poolSizeCount
        poolSizes.data(), // pPoolSizes
    };

    res = vkCreateDescriptorPool(vkdev, &descriptorPoolCreateInfo, 0, &m_descriptor_pool);

    if (res != VK_SUCCESS) {
        cvk_error("Could not create descriptor pool.");
        return CL_INVALID_VALUE;
    }

    return CL_SUCCESS;
}

bool cvk_kernel::setup_descriptor_set(VkDescriptorSet *ds, std::unique_ptr<cvk_mem> &pod_buffer)
{
    std::lock_guard<std::mutex> lock(m_lock);

    // Allocate descriptor sets
    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        nullptr,
        m_descriptor_pool,
        1, // descriptorSetCount
        &m_descriptor_set_layout
    };

    auto dev = m_context->device()->vulkan_device();

    VkResult res = vkAllocateDescriptorSets(dev, &descriptorSetAllocateInfo, ds);

    if (res != VK_SUCCESS) {
        cvk_error_fn("could not allocate descriptor sets: %s", vulkan_error_string(res));
        return false;
    }

    // Setup descriptors for POD arguments
    if (has_pod_arguments()) {

        // Transfer ownership of the POD buffer to the command
        pod_buffer = std::move(m_pod_buffer);

        // Allocate a new one
        if (!allocate_pod_buffer()) {
            return false;
        }

        // Update desciptors
        VkDescriptorBufferInfo bufferInfo = {
            pod_buffer->vulkan_buffer(),
            0, // offset
            VK_WHOLE_SIZE
        };

        VkWriteDescriptorSet writeDescriptorSet = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            nullptr,
            *ds,
            m_pod_binding, // dstBinding
            0, // dstArrayElement
            1, // descriptorCount
            m_pod_descriptor_type, // descriptorType
            nullptr, // pImageInfo
            &bufferInfo,
            nullptr, // pTexelBufferView
        };
        vkUpdateDescriptorSets(dev, 1, &writeDescriptorSet, 0, nullptr);
    }

    // Setup other descriptors
    for (cl_uint i = 0; i < m_args.size(); i++) {

        auto const &arg = m_args[i];
        cvk_kernel_arg_storage *storage = &m_args_storage[i];

        switch (arg.kind){

        case kernel_argument_kind::buffer: {
            cvk_mem *mem = *reinterpret_cast<cvk_mem*const*>(storage->ptr);
            mem->retain(); // FIXME per command with release
            VkBuffer buffer = mem->vulkan_buffer();
            cvk_debug_fn("buffer = %p", buffer);
            VkDescriptorBufferInfo bufferInfo = {
                buffer,
                0, // offset
                VK_WHOLE_SIZE
            };

            VkWriteDescriptorSet writeDescriptorSet = {
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                nullptr,
                *ds,
                arg.binding, // dstBinding
                0, // dstArrayElement
                1, // descriptorCount
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, // pImageInfo
                &bufferInfo,
                nullptr, // pTexelBufferView
            };
            vkUpdateDescriptorSets(dev, 1, &writeDescriptorSet, 0, nullptr);
            break;
        }
        case kernel_argument_kind::pod: // skip POD arguments
        case kernel_argument_kind::pod_ubo:
            break;
        default:
            cvk_error_fn("unsupported argument type");
            return false;
        }
    }

    return true;
}

cl_int cvk_kernel::set_arg(cl_uint index, size_t size, const void *value)
{
    auto const &arg = m_args[index];
    cvk_kernel_arg_storage *storage = &m_args_storage[index];

    std::lock_guard<std::mutex> lock(m_lock);

    cl_int err = CL_SUCCESS;

    if (arg.is_pod()) {
        if (!m_pod_buffer->copy_from(value, arg.offset, size)) { // FIXME check size, this allows overwrites
            err = CL_OUT_OF_RESOURCES;
        }
    } else {
        if (size > storage->size) {
            cvk_warn_fn("app trying to store more than the argument can receive, will be truncated");
        }

        memcpy(storage->ptr, value, std::min(size, storage->size));
    }

    return err;
}
