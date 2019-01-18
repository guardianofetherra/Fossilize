/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "volk.h"
#include "device.hpp"
#include "fossilize.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "file.hpp"
#include "path.hpp"
#include "fossilize_db.hpp"

#include <cinttypes>
#include <string>
#include <unordered_set>
#include <stdlib.h>
#include <dirent.h>	// VALVE
#include <chrono>	// VALVE
#include <queue>	// VALVE
#include <thread>	// VALVE
#include <mutex>	// VALVE
#include <condition_variable> // VALVE
#include <fstream>

using namespace Fossilize;
using namespace std;

struct DumbReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;

		// VALVE: Add multi-threaded pipeline creation
		uint32_t num_threads = thread::hardware_concurrency();

		// VALVE: --loop option for testing performance
		int32_t loop_count = 0;
	};

public:
	
	// VALVE: Kick off threads to perform pipeline compiles
	void drainWorkQueue()
	{
		int32_t nPipelineCount = ( int32_t ) pipelineWorkQueue.size();
		std::queue< PipelineWorkItem_t > pipelineWorkQueueCopy;
		if ( nLoopCount > 0 )
		{
			pipelineWorkQueueCopy = pipelineWorkQueue;
		}

		// Create a thread pool with the # of specified worker threads (defaults to thread::hardware_concurrency()).
		std::vector< std::thread > threadPool;
		for ( int32_t i = 0; i < numWorkerThreads; i++ )
		{
			threadPool.push_back( std::thread( &DumbReplayer::workerThreadRun, this ) );
		}

		auto start_time = chrono::steady_clock::now();
		do 
		{
			pipelineWorkQueueMutex.lock();
			while ( !pipelineWorkQueue.empty() )
			{
				pipelineWorkQueueMutex.unlock();
				workAvailableCondition.notify_all();
				pipelineWorkQueueMutex.lock();
			}
			pipelineWorkQueueMutex.unlock();
			
			unsigned long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();
			LOGI( "Compiling %d pipelines took %lu ms (avg: %0.2f ms / pipeline)\n", nPipelineCount, elapsed_ms, ( float ) elapsed_ms / ( float ) nPipelineCount );

			// For perf testing in --loop mode
			if ( nLoopCount > 0 )
			{
				std::lock_guard< std::mutex> lock( pipelineWorkQueueMutex );
				// Copy all the work back on to the queue
				pipelineWorkQueue = pipelineWorkQueueCopy;
				nLoopCount--;

				// Free all pipelines so we don't keep growing
				for ( auto it = graphics_pipelines.begin(); it != graphics_pipelines.end(); it++ )
				{
					if ( it->second != VK_NULL_HANDLE )
					{
						vkDestroyPipeline( device.get_device(), it->second, nullptr );
						it->second = VK_NULL_HANDLE;
					}
				}
				graphics_pipelines.clear();

				// Free all pipelines so we don't keep growing
				for ( auto it = compute_pipelines.begin(); it != compute_pipelines.end(); it++ )
				{
					if ( it->second != VK_NULL_HANDLE )
					{
						vkDestroyPipeline( device.get_device(), it->second, nullptr );
						it->second = VK_NULL_HANDLE;
					}
				}
				compute_pipelines.clear();


			}
			start_time = chrono::steady_clock::now();

		} while ( nLoopCount > 0 );

		bShuttingDown = true;
		workAvailableCondition.notify_all();
		for ( int32_t i = 0; i < numWorkerThreads; i++ )
		{
			threadPool[ i ].join();
		}
	}

	// VALVE: Worker thread function - grab work item off of the queue and perform the compile
	void workerThreadRun()
	{
		// If user asked for a pipeline cache, create a pipeline cache on each thread
		VkPipelineCache perThreadPipelineCache = VK_NULL_HANDLE;
		if ( pipeline_cache != VK_NULL_HANDLE )
		{
			VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
			vkCreatePipelineCache( device.get_device(), &info, nullptr, &perThreadPipelineCache );
		}

		while ( !bShuttingDown )
		{
			PipelineWorkItem_t workItem;
			{
				std::unique_lock< std::mutex > lock( pipelineWorkQueueMutex );
				workAvailableCondition.wait( lock, [&] 
					{
						if ( bShuttingDown )
						{
							return true;
						}
						if ( !pipelineWorkQueue.empty() )
						{
							workItem = pipelineWorkQueue.front();
							pipelineWorkQueue.pop();
							return true;
						}
						return false;
					} );
			}

			if ( bShuttingDown )
				continue;
			
			// Create graphics or compute pipeline
			if ( workItem.pGraphicsPipelineCreateInfo )
			{
				if ( vkCreateGraphicsPipelines( device.get_device(), perThreadPipelineCache, 1, workItem.pGraphicsPipelineCreateInfo, nullptr, workItem.ppPipeline ) != VK_SUCCESS)
				{
					LOGE("Creating graphics pipeline %0" PRIX64 " failed\n", workItem.index);
				}
				else
				{
					// Insert back into the map - needs a lock to be thread safe
					std::lock_guard< std::mutex > lock( pipelineWorkQueueMutex );
					graphics_pipelines[ workItem.index ] = *workItem.ppPipeline;
				}
			}
			else if ( workItem.pComputePipelineCreateInfo )
			{
				if ( vkCreateComputePipelines( device.get_device(), perThreadPipelineCache, 1, workItem.pComputePipelineCreateInfo, nullptr, workItem.ppPipeline ) != VK_SUCCESS)
				{
					LOGE("Creating compute pipeline %0" PRIX64 " failed\n", workItem.index);
				}
				else
				{
					// Insert back into the map - needs a lock to be thread safe
					std::lock_guard< std::mutex > lock( pipelineWorkQueueMutex );
					compute_pipelines[ workItem.index ] = *workItem.ppPipeline;
				}
			}
		}

		// Merge worker thread pipeline cache to the main cache
		if ( perThreadPipelineCache != VK_NULL_HANDLE )
		{
			{
				std::lock_guard< std::mutex > lock( pipelineWorkQueueMutex );
				vkMergePipelineCaches( device.get_device(), pipeline_cache, 1, &perThreadPipelineCache );
			}
			vkDestroyPipelineCache( device.get_device(), perThreadPipelineCache, nullptr );
		}
	}

	DumbReplayer(const VulkanDevice::Options &device_opts_, const Options &opts_,
	             const unordered_set<unsigned> &graphics,
	             const unordered_set<unsigned> &compute)
		: opts(opts_), filter_graphics(graphics), filter_compute(compute), numWorkerThreads( opts.num_threads ), nLoopCount( opts.loop_count ),bShuttingDown( false ), device_opts(device_opts_)
	{
	}

	~DumbReplayer()
	{
		if (pipeline_cache)
			vkDestroyPipelineCache(device.get_device(), pipeline_cache, nullptr);
		for (auto &sampler : samplers)
			if (sampler.second)
				vkDestroySampler(device.get_device(), sampler.second, nullptr);
		for (auto &layout : layouts)
			if (layout.second)
				vkDestroyDescriptorSetLayout(device.get_device(), layout.second, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout.second)
				vkDestroyPipelineLayout(device.get_device(), pipeline_layout.second, nullptr);
		for (auto &shader_module : shader_modules)
			if (shader_module.second)
				vkDestroyShaderModule(device.get_device(), shader_module.second, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass.second)
				vkDestroyRenderPass(device.get_device(), render_pass.second, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device.get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device.get_device(), pipeline.second, nullptr);
	}

	void set_application_info(const VkApplicationInfo *app, const VkPhysicalDeviceFeatures2 *features) override
	{
		// TODO: Could use this to create multiple VkDevices for replay as necessary if app changes.

		if (!device_was_init)
		{
			// Now we can init the device with correct app info.
			device_was_init = true;
			device_opts.application_info = app;
			device_opts.features = features;
			if (!device.init_device(device_opts))
			{
				LOGE("Failed to create Vulkan device, bailing ...\n");
				exit(EXIT_FAILURE);
			}

			if (opts.pipeline_cache)
			{
				VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
				vkCreatePipelineCache(device.get_device(), &info, nullptr, &pipeline_cache);
			}

			if (app)
			{
				LOGI("Replaying for application:\n");
				LOGI("  apiVersion: %u.%u.%u\n",
				     VK_VERSION_MAJOR(app->apiVersion),
				     VK_VERSION_MINOR(app->apiVersion),
				     VK_VERSION_PATCH(app->apiVersion));
				LOGI("  engineVersion: %u\n", app->engineVersion);
				LOGI("  applicationVersion: %u\n", app->applicationVersion);
				if (app->pEngineName)
					LOGI("  engineName: %s\n", app->pEngineName);
				if (app->pApplicationName)
					LOGI("  applicationName: %s\n", app->pApplicationName);
			}
		}
	}

	bool enqueue_create_sampler(Hash index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		if (vkCreateSampler(device.get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %0" PRIX64 " Failed!\n", index);
			return false;
		}
		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		if (vkCreateDescriptorSetLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating descriptor set layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		if (vkCreatePipelineLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating pipeline layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		pipeline_layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_shader_module(Hash index, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		if (vkCreateShaderModule(device.get_device(), create_info, nullptr, module) != VK_SUCCESS)
		{
			LOGE("Creating shader module %0" PRIX64 " Failed!\n", index);
			return false;
		}
		shader_modules[index] = *module;
		return true;
	}

	bool enqueue_create_render_pass(Hash index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		if (vkCreateRenderPass(device.get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
		{
			LOGE("Creating render pass %0" PRIX64 " Failed!\n", index);
			return false;
		}
		render_passes[index] = *render_pass;
		return true;
	}

	bool enqueue_create_compute_pipeline(Hash index, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if ((filter_compute.empty() && filter_graphics.empty()) || filter_compute.count(index))
		{
			PipelineWorkItem_t workItem( index );
			workItem.pComputePipelineCreateInfo = create_info;
			workItem.ppPipeline = pipeline;
			pipelineWorkQueue.push( workItem );
		}
		else
			*pipeline = VK_NULL_HANDLE;

		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash index, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if ((filter_graphics.empty() && filter_compute.empty()) || filter_graphics.count(index))
		{
			PipelineWorkItem_t workItem( index );
			workItem.pGraphicsPipelineCreateInfo = create_info;
			workItem.ppPipeline = pipeline;
			pipelineWorkQueue.push( workItem );
			
		}
		else
			*pipeline = VK_NULL_HANDLE;

		return true;
	}

	Options opts;
	const unordered_set<unsigned> &filter_graphics;
	const unordered_set<unsigned> &filter_compute;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> shader_modules;
	std::unordered_map<Hash, VkRenderPass> render_passes;
	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

	// VALVE: multi-threaded work queue for replayer
	struct PipelineWorkItem_t
	{
		PipelineWorkItem_t() :
			PipelineWorkItem_t( Hash() )
		{
		}
		PipelineWorkItem_t( Hash idx ) : 
			index( idx ),
			pGraphicsPipelineCreateInfo( nullptr ),
			pComputePipelineCreateInfo( nullptr ),
			ppPipeline( nullptr )
		{
		}

		Hash index;
		const VkGraphicsPipelineCreateInfo *pGraphicsPipelineCreateInfo;
		const VkComputePipelineCreateInfo *pComputePipelineCreateInfo;
		VkPipeline *ppPipeline;
	};
	int32_t numWorkerThreads;
	int32_t nLoopCount;
	std::mutex pipelineWorkQueueMutex;
	std::queue< PipelineWorkItem_t > pipelineWorkQueue;
	std::condition_variable workAvailableCondition;
	volatile bool bShuttingDown;

	VulkanDevice device;
	bool device_was_init = false;
	VulkanDevice::Options device_opts;
};

static void print_help()
{
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--pipeline-cache]\n"
	     "\t[--filter-compute <index>]\n"
	     "\t[--filter-graphics <index>]\n"
	     "\t[--num-threads <count>]\n"
	     "\t[--loop <count>]\n"
	     "\t<JSON directory>\n");
}

// VALVE: Modified to not use std::filesystem
static std::vector<uint8_t> load_buffer_from_path(const std::string &path)
{
	FILE *file = fopen(path.c_str(), "rb");
	if (!file)
	{
		LOGE("Failed to open file: %s\n", path.c_str());
		return {};
	}

	if (fseek(file, 0, SEEK_END) < 0)
	{
		fclose(file);
		LOGE("Failed to seek in file: %s\n", path.c_str());
		return {};
	}

	size_t file_size = size_t(ftell(file));
	rewind(file);

	std::vector<uint8_t> file_data(file_size);
	if (fread(file_data.data(), 1, file_size, file) != file_size)
	{
		LOGE("Failed to read from file: %s\n", path.c_str());
		fclose(file);
		return {};
	}

	fclose(file);
	return file_data;
}

int main(int argc, char *argv[])
{
	string json_path;
	VulkanDevice::Options opts;
	DumbReplayer::Options replayer_opts;

	unordered_set<unsigned> filter_graphics;
	unordered_set<unsigned> filter_compute;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { json_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
	cbs.add("--filter-compute", [&](CLIParser &parser) { filter_compute.insert(parser.next_uint()); });
	cbs.add("--filter-graphics", [&](CLIParser &parser) { filter_graphics.insert(parser.next_uint()); });
	cbs.add("--num-threads", [&](CLIParser &parser) { replayer_opts.num_threads = parser.next_uint(); });
	cbs.add("--loop", [&](CLIParser &parser) { replayer_opts.loop_count = parser.next_uint(); });
	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (json_path.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	// VALVE: modified to not use std::filesystem
	struct dirent *pEntry;
	DIR *dp;
	dp = opendir( json_path.c_str() );
	if ( dp == NULL )
	{
		LOGE("Invalid path to serialized state provided.\n");
		return EXIT_FAILURE;
	}

	auto start_time = chrono::steady_clock::now();

	DumbReplayer replayer(opts, replayer_opts, filter_graphics, filter_compute);
	auto resolver = create_dumb_folder_database(json_path);
	StateReplayer state_replayer;

	// VALVE: modified to not use std::filesystem
	while ( ( pEntry = readdir( dp ) ) )
	{
		if ( pEntry->d_type != DT_REG)
			continue;

		// VALVE: modified to not use std::filesystem
		std::string path( pEntry->d_name );
		auto dotpos = path.find(".");
		if (dotpos == std::string::npos)
			continue;

		std::string stem = path.substr( 0, dotpos );
		std::string ext = path.substr( dotpos );
		// check that filename is 16 char hex with json extension
		if (stem.length() != 16)
			continue;
		for (auto c : stem)
		{
			if (!isxdigit(c))
				continue;
		}
		if (ext != ".json")
			continue;

		try
		{
			auto state_json = load_buffer_from_path(Path::join(json_path, path));
			if (state_json.empty())
			{
				LOGE("Failed to load %s from disk.\n", pEntry->d_name);
			}

			state_replayer.parse(replayer, resolver.get(), state_json.data(), state_json.size());
		}
		catch (const exception &e)
		{
			LOGE("StateReplayer threw exception parsing %s: %s\n", pEntry->d_name, e.what());
		}
	}

	// VALVE: drain all outstanding pipeline compiles
	replayer.drainWorkQueue();
	
	// VALVE: modified to not use std::filesystem
	closedir( dp );

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.size() +
		replayer.render_passes.size() +
		replayer.compute_pipelines.size() +
		replayer.graphics_pipelines.size();

	unsigned long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Replayed %lu objects in %lu ms:\n", total_size, elapsed_ms);
	LOGI("  samplers:              %7lu\n", (unsigned long)replayer.samplers.size());
	LOGI("  descriptor set layouts:%7lu\n", (unsigned long)replayer.layouts.size());
	LOGI("  pipeline layouts:      %7lu\n", (unsigned long)replayer.pipeline_layouts.size());
	LOGI("  shader modules:        %7lu\n", (unsigned long)replayer.shader_modules.size());
	LOGI("  render passes:         %7lu\n", (unsigned long)replayer.render_passes.size());
	LOGI("  compute pipelines:     %7lu\n", (unsigned long)replayer.compute_pipelines.size());
	LOGI("  graphics pipelines:    %7lu\n", (unsigned long)replayer.graphics_pipelines.size());

	return EXIT_SUCCESS;
}
