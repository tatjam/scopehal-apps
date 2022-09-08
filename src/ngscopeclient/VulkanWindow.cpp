/***********************************************************************************************************************
*                                                                                                                      *
* glscopeclient                                                                                                        *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of VulkanWindow
 */
#include "ngscopeclient.h"
#include "VulkanWindow.h"
#include "VulkanFFTPlan.h"

using namespace std;

#define IMAGE_COUNT 2

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

/**
	@brief Creates a new top level window with the specified title
 */
VulkanWindow::VulkanWindow(const string& title, vk::raii::Queue& queue)
	: m_renderQueue(queue)
{
	//Don't configure Vulkan or center the mouse
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_CENTER_CURSOR, GLFW_FALSE);

	//Create the window
	m_window = glfwCreateWindow(1280, 720, title.c_str(), nullptr, nullptr);
	if(!m_window)
	{
		LogError("Window creation failed\n");
		abort();
	}

	//Create a Vulkan surface for drawing onto
	VkSurfaceKHR surface;
	if(VK_SUCCESS != glfwCreateWindowSurface(**g_vkInstance, m_window, nullptr, &surface))
	{
		LogError("Vulkan surface creation failed\n");
		abort();
	}

	//Encapsulate the generated surface in a C++ object for easier access
	m_surface = make_shared<vk::raii::SurfaceKHR>(*g_vkInstance, surface);
	m_wdata.Surface = **m_surface;

	//Make a descriptor pool for ImGui
	//TODO: tune sizes?
	const int numImguiDescriptors = 1000;
	vector<vk::DescriptorPoolSize> poolSizes;
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eSampler, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eSampledImage, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eUniformTexelBuffer, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eStorageTexelBuffer, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eUniformBuffer, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eUniformBufferDynamic, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eStorageBufferDynamic, numImguiDescriptors));
	poolSizes.push_back(vk::DescriptorPoolSize(vk::DescriptorType::eInputAttachment, numImguiDescriptors));
	vk::DescriptorPoolCreateInfo poolInfo(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, poolSizes);
	m_imguiDescriptorPool = make_unique<vk::raii::DescriptorPool>(*g_vkComputeDevice, poolInfo);

	UpdateFramebuffer();

	//Initialize ImGui
	ImGui_ImplGlfw_InitForVulkan(m_window, true);
	ImGui_ImplVulkan_InitInfo info = {};
	info.Instance = **g_vkInstance;
	info.PhysicalDevice = **g_vkfftPhysicalDevice;
	info.Device = **g_vkComputeDevice;
	info.QueueFamily = g_renderQueueType;
	info.PipelineCache = **g_pipelineCacheMgr->Lookup("ImGui.spv", IMGUI_VERSION_NUM);
	info.DescriptorPool = **m_imguiDescriptorPool;
	info.Subpass = 0;
	info.MinImageCount = IMAGE_COUNT;
	info.ImageCount = m_wdata.ImageCount;
	info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	info.Queue = *queue;
	ImGui_ImplVulkan_Init(&info, m_wdata.RenderPass);
}

//temporary prototype for internal function we should probably not use long term
void ImGui_ImplVulkanH_DestroyFrame(VkDevice device, ImGui_ImplVulkanH_Frame* fd, const VkAllocationCallbacks* allocator);
void ImGui_ImplVulkanH_DestroyFrameSemaphores(VkDevice device, ImGui_ImplVulkanH_FrameSemaphores* fsd, const VkAllocationCallbacks* allocator);

/**
	@brief Destroys a VulkanWindow
 */
VulkanWindow::~VulkanWindow()
{
	for (uint32_t i = 0; i < m_wdata.ImageCount; i++)
    {
        ImGui_ImplVulkanH_DestroyFrame(**g_vkComputeDevice, &m_wdata.Frames[i], nullptr);
        ImGui_ImplVulkanH_DestroyFrameSemaphores(**g_vkComputeDevice, &m_wdata.FrameSemaphores[i], nullptr);
    }
    IM_FREE(m_wdata.Frames);
    IM_FREE(m_wdata.FrameSemaphores);
    m_wdata.Frames = NULL;
    m_wdata.FrameSemaphores = NULL;
    vkDestroyPipeline(**g_vkComputeDevice, m_wdata.Pipeline, VK_NULL_HANDLE);
    vkDestroyRenderPass(**g_vkComputeDevice, m_wdata.RenderPass, VK_NULL_HANDLE);
    vkDestroySwapchainKHR(**g_vkComputeDevice, m_wdata.Swapchain, VK_NULL_HANDLE);
    m_wdata = ImGui_ImplVulkanH_Window();

	m_surface = nullptr;
	glfwDestroyWindow(m_window);
	m_imguiDescriptorPool = nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Rendering

void VulkanWindow::UpdateFramebuffer()
{
	//Figure out how big our framebuffer is
	int width;
	int height;
	glfwGetFramebufferSize(m_window, &width, &height);
	LogDebug("Framebuffer size: %d x %d\n", width, height);

	const VkFormat requestSurfaceImageFormat[] =
	{
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8_UNORM,
		VK_FORMAT_R8G8B8_UNORM
	};
	const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
	m_wdata.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
		**g_vkfftPhysicalDevice,
		m_wdata.Surface,
		requestSurfaceImageFormat,
		(size_t)IM_ARRAYSIZE(requestSurfaceImageFormat),
		requestSurfaceColorSpace);
	m_wdata.PresentMode = VK_PRESENT_MODE_FIFO_KHR;

	// Create SwapChain, RenderPass, Framebuffer, etc.
	ImGui_ImplVulkanH_CreateOrResizeWindow(
		**g_vkInstance,
		**g_vkfftPhysicalDevice,
		**g_vkComputeDevice,
		&m_wdata,
		g_renderQueueType,
		nullptr,
		width,
		height,
		IMAGE_COUNT);
}

void VulkanWindow::Render()
{
	ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

	//Start frame
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	bool show = true;
	ImGui::ShowDemoWindow(&show);

	//Do the actual render
	ImGui::Render();
	ImDrawData* main_draw_data = ImGui::GetDrawData();
	const bool main_is_minimized = (main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f);
	m_wdata.ClearValue.color.float32[0] = clear_color.x * clear_color.w;
	m_wdata.ClearValue.color.float32[1] = clear_color.y * clear_color.w;
	m_wdata.ClearValue.color.float32[2] = clear_color.z * clear_color.w;
	m_wdata.ClearValue.color.float32[3] = clear_color.w;
	if (!main_is_minimized)
	{
		VkResult err;

		VkSemaphore image_acquired_semaphore  = m_wdata.FrameSemaphores[m_wdata.SemaphoreIndex].ImageAcquiredSemaphore;
		VkSemaphore render_complete_semaphore = m_wdata.FrameSemaphores[m_wdata.SemaphoreIndex].RenderCompleteSemaphore;
		err = vkAcquireNextImageKHR(**g_vkComputeDevice, m_wdata.Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &m_wdata.FrameIndex);
		if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
		{
			//g_SwapChainRebuild = true;
			return;
		}
		//check_vk_result(err);

		ImGui_ImplVulkanH_Frame* fd = &m_wdata.Frames[m_wdata.FrameIndex];
		{
			err = vkWaitForFences(**g_vkComputeDevice, 1, &fd->Fence, VK_TRUE, UINT64_MAX);    // wait indefinitely instead of periodically checking
			//check_vk_result(err);

			err = vkResetFences(**g_vkComputeDevice, 1, &fd->Fence);
			//check_vk_result(err);
		}
		{
			err = vkResetCommandPool(**g_vkComputeDevice, fd->CommandPool, 0);
			//check_vk_result(err);
			VkCommandBufferBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
			//check_vk_result(err);
		}
		{
			VkRenderPassBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			info.renderPass = m_wdata.RenderPass;
			info.framebuffer = fd->Framebuffer;
			info.renderArea.extent.width = m_wdata.Width;
			info.renderArea.extent.height = m_wdata.Height;
			info.clearValueCount = 1;
			info.pClearValues = &m_wdata.ClearValue;
			vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
		}

		// Record dear imgui primitives into command buffer
		ImGui_ImplVulkan_RenderDrawData(main_draw_data, fd->CommandBuffer);

		// Submit command buffer
		vkCmdEndRenderPass(fd->CommandBuffer);
		{
			VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			VkSubmitInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			info.waitSemaphoreCount = 1;
			info.pWaitSemaphores = &image_acquired_semaphore;
			info.pWaitDstStageMask = &wait_stage;
			info.commandBufferCount = 1;
			info.pCommandBuffers = &fd->CommandBuffer;
			info.signalSemaphoreCount = 1;
			info.pSignalSemaphores = &render_complete_semaphore;

			err = vkEndCommandBuffer(fd->CommandBuffer);
			//check_vk_result(err);
			err = vkQueueSubmit(*m_renderQueue, 1, &info, fd->Fence);
			//check_vk_result(err);
		}
	}

	// Update and Render additional Platform Windows
	ImGui::UpdatePlatformWindows();
	ImGui::RenderPlatformWindowsDefault();

	// Present Main Platform Window
	if (!main_is_minimized)
	{
		VkSemaphore render_complete_semaphore = m_wdata.FrameSemaphores[m_wdata.SemaphoreIndex].RenderCompleteSemaphore;
		VkPresentInfoKHR info = {};
		info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &render_complete_semaphore;
		info.swapchainCount = 1;
		info.pSwapchains = &m_wdata.Swapchain;
		info.pImageIndices = &m_wdata.FrameIndex;
		VkResult err = vkQueuePresentKHR(*m_renderQueue, &info);
		if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
		{
			//g_SwapChainRebuild = true;
			return;
		}
		m_wdata.SemaphoreIndex = (m_wdata.SemaphoreIndex + 1) % m_wdata.ImageCount; // Now we can use the next set of semaphores
	}
}
