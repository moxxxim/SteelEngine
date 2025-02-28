#include "Engine/Render/Stages/LightingStage.hpp"

#include "Engine/Render/RenderContext.hpp"
#include "Engine/Render/Stages/GBufferStage.hpp"
#include "Engine/Render/Vulkan/PipelineHelpers.hpp"
#include "Engine/Render/Vulkan/ComputePipeline.hpp"
#include "Engine/Render/Vulkan/VulkanContext.hpp"
#include "Engine/Render/Vulkan/Resources/BufferHelpers.hpp"
#include "Engine/Render/Vulkan/Resources/ImageHelpers.hpp"
#include "Engine/Scene/GlobalIllumination.hpp"
#include "Engine/Scene/ImageBasedLighting.hpp"
#include "Engine/Scene/StorageComponents.hpp"
#include "Engine/Scene/Environment.hpp"
#include "Engine/Scene/Components.hpp"
#include "Engine/Scene/Scene.hpp"

namespace Details
{
    static constexpr glm::uvec2 kWorkGroupSize(8, 8);

    static DescriptorSet CreateGBufferDescriptorSet(const std::vector<vk::ImageView>& imageViews)
    {
        const DescriptorDescription storageImageDescriptorDescription{
            1, vk::DescriptorType::eStorageImage,
            vk::ShaderStageFlagBits::eCompute,
            vk::DescriptorBindingFlags()
        };

        const DescriptorDescription sampledImageDescriptorDescription{
            1, vk::DescriptorType::eCombinedImageSampler,
            vk::ShaderStageFlagBits::eCompute,
            vk::DescriptorBindingFlags()
        };

        DescriptorSetDescription descriptorSetDescription(imageViews.size());
        DescriptorSetData descriptorSetData(imageViews.size());

        for (size_t i = 0; i < imageViews.size(); ++i)
        {
            if (ImageHelpers::IsDepthFormat(GBufferStage::kFormats[i]))
            {
                descriptorSetDescription[i] = sampledImageDescriptorDescription;
                descriptorSetData[i] = DescriptorHelpers::GetData(RenderContext::texelSampler, imageViews[i]);
            }
            else
            {
                descriptorSetDescription[i] = storageImageDescriptorDescription;
                descriptorSetData[i] = DescriptorHelpers::GetStorageData(imageViews[i]);
            }
        }

        return DescriptorHelpers::CreateDescriptorSet(descriptorSetDescription, descriptorSetData);
    }

    static MultiDescriptorSet CreateSwapchainDescriptorSet()
    {
        const std::vector<vk::ImageView>& swapchainImageViews = VulkanContext::swapchain->GetImageViews();

        const DescriptorDescription descriptorDescription{
            1, vk::DescriptorType::eStorageImage,
            vk::ShaderStageFlagBits::eCompute,
            vk::DescriptorBindingFlags()
        };

        std::vector<DescriptorSetData> multiDescriptorSetData;
        multiDescriptorSetData.reserve(swapchainImageViews.size());

        for (const auto& swapchainImageView : swapchainImageViews)
        {
            multiDescriptorSetData.push_back({ DescriptorHelpers::GetStorageData(swapchainImageView) });
        }

        return DescriptorHelpers::CreateMultiDescriptorSet({ descriptorDescription }, multiDescriptorSetData);
    }

    static CameraData CreateCameraData()
    {
        const uint32_t bufferCount = VulkanContext::swapchain->GetImageCount();

        constexpr vk::DeviceSize bufferSize = sizeof(glm::mat4);

        constexpr vk::ShaderStageFlags shaderStages = vk::ShaderStageFlagBits::eCompute;

        return RenderHelpers::CreateCameraData(bufferCount, bufferSize, shaderStages);
    }

    static DescriptorSet CreateLightingDescriptorSet(const Scene& scene)
    {
        const auto& environmentComponent = scene.ctx().get<EnvironmentComponent>();
        const auto& renderComponent = scene.ctx().get<RenderStorageComponent>();

        const ImageBasedLighting& imageBasedLighting = *RenderContext::imageBasedLighting;

        const ImageBasedLighting::Samplers& iblSamplers = imageBasedLighting.GetSamplers();

        const Texture& irradianceTexture = environmentComponent.irradianceTexture;
        const Texture& reflectionTexture = environmentComponent.reflectionTexture;
        const Texture& specularBRDF = imageBasedLighting.GetSpecularBRDF();

        DescriptorSetDescription descriptorSetDescription{
            DescriptorDescription{
                1, vk::DescriptorType::eCombinedImageSampler,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
            DescriptorDescription{
                1, vk::DescriptorType::eCombinedImageSampler,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
            DescriptorDescription{
                1, vk::DescriptorType::eCombinedImageSampler,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
            DescriptorDescription{
                1, vk::DescriptorType::eUniformBuffer,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
        };

        DescriptorSetData descriptorSetData{
            DescriptorHelpers::GetData(iblSamplers.irradiance, irradianceTexture.view),
            DescriptorHelpers::GetData(iblSamplers.reflection, reflectionTexture.view),
            DescriptorHelpers::GetData(iblSamplers.specularBRDF, specularBRDF.view),
            DescriptorHelpers::GetData(renderComponent.lightBuffer),
        };

        if (scene.ctx().contains<LightVolumeComponent>())
        {
            const auto& lightVolumeComponent = scene.ctx().get<LightVolumeComponent>();

            descriptorSetDescription.push_back(DescriptorDescription{
                1, vk::DescriptorType::eStorageBuffer,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            });
            descriptorSetDescription.push_back(DescriptorDescription{
                1, vk::DescriptorType::eStorageBuffer,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            });
            descriptorSetDescription.push_back(DescriptorDescription{
                1, vk::DescriptorType::eStorageBuffer,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            });

            descriptorSetData.push_back(DescriptorHelpers::GetStorageData(lightVolumeComponent.positionsBuffer));
            descriptorSetData.push_back(DescriptorHelpers::GetStorageData(lightVolumeComponent.tetrahedralBuffer));
            descriptorSetData.push_back(DescriptorHelpers::GetStorageData(lightVolumeComponent.coefficientsBuffer));
        }

        return DescriptorHelpers::CreateDescriptorSet(descriptorSetDescription, descriptorSetData);
    }

    static DescriptorSet CreateRayTracingDescriptorSet(const Scene& scene)
    {
        const auto& rayTracingComponent = scene.ctx().get<RayTracingStorageComponent>();
        const auto& textureComponent = scene.ctx().get<TextureStorageComponent>();
        const auto& renderComponent = scene.ctx().get<RenderStorageComponent>();

        const uint32_t textureCount = static_cast<uint32_t>(textureComponent.textures.size());
        const uint32_t primitiveCount = static_cast<uint32_t>(rayTracingComponent.blases.size());

        const DescriptorSetDescription descriptorSetDescription{
            DescriptorDescription{
                1, vk::DescriptorType::eAccelerationStructureKHR,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
            DescriptorDescription{
                1, vk::DescriptorType::eUniformBuffer,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
            DescriptorDescription{
                textureCount, vk::DescriptorType::eCombinedImageSampler,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
            DescriptorDescription{
                primitiveCount, vk::DescriptorType::eStorageBuffer,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            },
            DescriptorDescription{
                primitiveCount, vk::DescriptorType::eStorageBuffer,
                vk::ShaderStageFlagBits::eCompute,
                vk::DescriptorBindingFlags()
            }
        };

        const DescriptorSetData descriptorSetData{
            DescriptorHelpers::GetData(renderComponent.tlas),
            DescriptorHelpers::GetData(renderComponent.materialBuffer),
            DescriptorHelpers::GetData(textureComponent.textures),
            DescriptorHelpers::GetStorageData(rayTracingComponent.indexBuffers),
            DescriptorHelpers::GetStorageData(rayTracingComponent.vertexBuffers),
        };

        return DescriptorHelpers::CreateDescriptorSet(descriptorSetDescription, descriptorSetData);
    }

    static std::unique_ptr<ComputePipeline> CreatePipeline(const Scene& scene,
            const std::vector<vk::DescriptorSetLayout>& descriptorSetLayouts)
    {
        const auto& materialComponent = scene.ctx().get<MaterialStorageComponent>();

        const bool lightVolumeEnabled = scene.ctx().contains<LightVolumeComponent>();

        const uint32_t materialCount = static_cast<uint32_t>(materialComponent.materials.size());

        const std::tuple specializationValues = std::make_tuple(
                kWorkGroupSize.x, kWorkGroupSize.y, materialCount);

        const ShaderDefines defines{
            std::make_pair("LIGHT_COUNT", static_cast<uint32_t>(scene.view<LightComponent>().size())),
            std::make_pair("RAY_TRACING_ENABLED", static_cast<uint32_t>(Config::kRayTracingEnabled)),
            std::make_pair("LIGHT_VOLUME_ENABLED", static_cast<uint32_t>(lightVolumeEnabled)),
        };

        const ShaderModule shaderModule = VulkanContext::shaderManager->CreateShaderModule(
                vk::ShaderStageFlagBits::eCompute, Filepath("~/Shaders/Hybrid/Lighting.comp"),
                defines, specializationValues);

        const vk::PushConstantRange pushConstantRange(
                vk::ShaderStageFlagBits::eCompute, 0, sizeof(glm::vec3));

        const ComputePipeline::Description description{
            shaderModule, descriptorSetLayouts, { pushConstantRange }
        };

        std::unique_ptr<ComputePipeline> pipeline = ComputePipeline::Create(description);

        VulkanContext::shaderManager->DestroyShaderModule(shaderModule);

        return pipeline;
    }
}

LightingStage::LightingStage(const std::vector<vk::ImageView>& gBufferImageViews)
{
    gBufferDescriptorSet = Details::CreateGBufferDescriptorSet(gBufferImageViews);

    swapchainDescriptorSet = Details::CreateSwapchainDescriptorSet();

    cameraData = Details::CreateCameraData();
}

LightingStage::~LightingStage()
{
    RemoveScene();

    DescriptorHelpers::DestroyMultiDescriptorSet(cameraData.descriptorSet);
    for (const auto& buffer : cameraData.buffers)
    {
        VulkanContext::bufferManager->DestroyBuffer(buffer);
    }

    DescriptorHelpers::DestroyDescriptorSet(gBufferDescriptorSet);
    DescriptorHelpers::DestroyMultiDescriptorSet(swapchainDescriptorSet);
}

void LightingStage::RegisterScene(const Scene* scene_)
{
    RemoveScene();

    scene = scene_;

    lightingDescriptorSet = Details::CreateLightingDescriptorSet(*scene);

    if constexpr (Config::kRayTracingEnabled)
    {
        rayTracingDescriptorSet = Details::CreateRayTracingDescriptorSet(*scene);
    }

    pipeline = Details::CreatePipeline(*scene, GetDescriptorSetLayouts());
}

void LightingStage::RemoveScene()
{
    if (!scene)
    {
        return;
    }

    pipeline.reset();

    DescriptorHelpers::DestroyDescriptorSet(lightingDescriptorSet);
    DescriptorHelpers::DestroyDescriptorSet(rayTracingDescriptorSet);

    scene = nullptr;
}

void LightingStage::Execute(vk::CommandBuffer commandBuffer, uint32_t imageIndex) const
{
    const auto& cameraComponent = scene->ctx().get<CameraComponent>();

    const glm::mat4& view = cameraComponent.viewMatrix;
    const glm::mat4& proj = cameraComponent.projMatrix;

    const glm::mat4 inverseProjView = glm::inverse(view) * glm::inverse(proj);

    BufferHelpers::UpdateBuffer(commandBuffer, cameraData.buffers[imageIndex],
            ByteView(inverseProjView), SyncScope::kWaitForNone, SyncScope::kComputeShaderRead);

    const vk::Image swapchainImage = VulkanContext::swapchain->GetImages()[imageIndex];
    const vk::Extent2D& extent = VulkanContext::swapchain->GetExtent();
    const glm::vec3& cameraPosition = cameraComponent.location.position;

    const ImageLayoutTransition layoutTransition{
        vk::ImageLayout::ePresentSrcKHR,
        vk::ImageLayout::eGeneral,
        PipelineBarrier{
            SyncScope::kWaitForNone,
            SyncScope::kComputeShaderWrite
        }
    };

    ImageHelpers::TransitImageLayout(commandBuffer, swapchainImage,
            ImageHelpers::kFlatColor, layoutTransition);

    std::vector<vk::DescriptorSet> descriptorSets{
        swapchainDescriptorSet.values[imageIndex],
        gBufferDescriptorSet.value,
        lightingDescriptorSet.value,
        cameraData.descriptorSet.values[imageIndex],
    };

    if constexpr (Config::kRayTracingEnabled)
    {
        descriptorSets.push_back(rayTracingDescriptorSet.value);
    }

    commandBuffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Get());

    commandBuffer.pushConstants<glm::vec3>(pipeline->GetLayout(),
            vk::ShaderStageFlagBits::eCompute, 0, { cameraPosition });

    commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
            pipeline->GetLayout(), 0, descriptorSets, {});

    const glm::uvec3 groupCount = PipelineHelpers::CalculateWorkGroupCount(extent, Details::kWorkGroupSize);

    commandBuffer.dispatch(groupCount.x, groupCount.y, groupCount.z);
}

void LightingStage::Resize(const std::vector<vk::ImageView>& gBufferImageViews)
{
    DescriptorHelpers::DestroyDescriptorSet(gBufferDescriptorSet);
    DescriptorHelpers::DestroyMultiDescriptorSet(swapchainDescriptorSet);

    gBufferDescriptorSet = Details::CreateGBufferDescriptorSet(gBufferImageViews);
    swapchainDescriptorSet = Details::CreateSwapchainDescriptorSet();

    pipeline = Details::CreatePipeline(*scene, GetDescriptorSetLayouts());
}

void LightingStage::ReloadShaders()
{
    pipeline = Details::CreatePipeline(*scene, GetDescriptorSetLayouts());
}

std::vector<vk::DescriptorSetLayout> LightingStage::GetDescriptorSetLayouts() const
{
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts{
        swapchainDescriptorSet.layout,
        gBufferDescriptorSet.layout,
        lightingDescriptorSet.layout,
        cameraData.descriptorSet.layout,
    };

    if constexpr (Config::kRayTracingEnabled)
    {
        descriptorSetLayouts.push_back(rayTracingDescriptorSet.layout);
    }

    return descriptorSetLayouts;
}
