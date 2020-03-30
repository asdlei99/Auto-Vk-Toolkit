#include <cg_base.hpp>

class ray_tracing_basic_usage_app : public cgb::cg_element
{
	struct push_const_data {
		glm::mat4 mViewMatrix;
		glm::vec4 mLightDirection;
	};

public: // v== cgb::cg_element overrides which will be invoked by the framework ==v

	void initialize() override
	{
		// Load an ORCA scene from file:
		auto orca = cgb::orca_scene_t::load_from_file("assets/sponza.fscene");
		// Iterate over all models, all model instances, and all meshes, and create bottom level acceleration structures for each one of them:
		for (const auto& modelData : orca->models()) {
			for (const auto& modelInstance : modelData.mInstances) {
				const auto& model = modelData.mLoadedModel;
				auto meshIndices = model->select_all_meshes();
				auto [vtxBfr, idxBfr] = cgb::get_combined_vertex_and_index_buffers_for_selected_meshes({ cgb::make_tuple_model_and_indices(model, meshIndices) });
				auto blas = cgb::bottom_level_acceleration_structure_t::create(std::move(vtxBfr), std::move(idxBfr));
				blas->build();
				mGeometryInstances.push_back(
					cgb::geometry_instance::create(blas)
						.set_transform(cgb::matrix_from_transforms(modelInstance.mTranslation, glm::quat(modelInstance.mRotation), modelInstance.mScaling))
				);
				mBLASs.push_back(std::move(blas));
			}
		}
		
		cgb::invoke_for_all_in_flight_frames(cgb::context().main_window(), [&](auto inFlightIndex){
			auto tlas = cgb::top_level_acceleration_structure_t::create(mGeometryInstances.size());
			tlas->build(mGeometryInstances);
			mTLAS.push_back(std::move(tlas));
		});
		
		// Create offscreen image views to ray-trace into, one for each frame in flight:
		size_t n = cgb::context().main_window()->number_of_in_flight_frames();
		const auto wdth = cgb::context().main_window()->resolution().x;
		const auto hght = cgb::context().main_window()->resolution().y;
		const auto frmt = cgb::image_format::from_window_color_buffer(cgb::context().main_window());
		cgb::invoke_for_all_in_flight_frames(cgb::context().main_window(), [&](auto inFlightIndex){
			mOffscreenImageViews.emplace_back(
				cgb::image_view_t::create(
					cgb::image_t::create(wdth, hght, frmt, false, 1, cgb::memory_usage::device, cgb::image_usage::versatile_image)
				)
			);
			mOffscreenImageViews.back()->get_image().transition_to_layout({}, cgb::sync::with_barriers_on_current_frame());
			assert((mOffscreenImageViews.back()->config().subresourceRange.aspectMask & vk::ImageAspectFlagBits::eColor) == vk::ImageAspectFlagBits::eColor);
		});

		// Create our ray tracing pipeline with the required configuration:
		mPipeline = cgb::ray_tracing_pipeline_for(
			cgb::define_shader_table(
				cgb::ray_generation_shader("shaders/ray_generation_shader.rgen"),
				cgb::triangles_hit_group::create_with_rchit_only("shaders/closest_hit_shader.rchit"),
				cgb::miss_shader("shaders/miss_shader.rmiss")
			),
			cgb::max_recursion_depth::set_to_max(),
			// Define push constants and descriptor bindings:
			cgb::push_constant_binding_data { cgb::shader_type::ray_generation | cgb::shader_type::closest_hit, 0, sizeof(push_const_data) },
			cgb::binding(0, 0, mOffscreenImageViews[0]), // Just take any, this is just to define the layout
			cgb::binding(1, 0, mTLAS[0])				 // Just take any, this is just to define the layout
		);

		// Add the camera to the composition (and let it handle the updates)
		mQuakeCam.set_translation({ 0.0f, 0.0f, 0.0f });
		mQuakeCam.set_perspective_projection(glm::radians(60.0f), cgb::context().main_window()->aspect_ratio(), 0.5f, 100.0f);
		//mQuakeCam.set_orthographic_projection(-5, 5, -5, 5, 0.5, 100);
		cgb::current_composition().add_element(mQuakeCam);
	}

	void update() override
	{
		static int counter = 0;
		if (++counter == 4) {
			auto current = std::chrono::high_resolution_clock::now();
			auto time_span = current - mInitTime;
			auto int_min = std::chrono::duration_cast<std::chrono::minutes>(time_span).count();
			auto int_sec = std::chrono::duration_cast<std::chrono::seconds>(time_span).count();
			auto fp_ms = std::chrono::duration<double, std::milli>(time_span).count();
			printf("Time from init to fourth frame: %d min, %lld sec %lf ms\n", int_min, int_sec - static_cast<decltype(int_sec)>(int_min) * 60, fp_ms - 1000.0 * int_sec);
		}

		if (cgb::input().key_pressed(cgb::key_code::space)) {
			// Print the current camera position
			auto pos = mQuakeCam.translation();
			LOG_INFO(fmt::format("Current camera position: {}", cgb::to_string(pos)));
		}

		if (cgb::input().key_pressed(cgb::key_code::escape)) {
			// Stop the current composition:
			cgb::current_composition().stop();
		}

		if (cgb::input().key_pressed(cgb::key_code::tab)) {
			if (mQuakeCam.is_enabled()) {
				mQuakeCam.disable();
			}
			else {
				mQuakeCam.enable();
			}
		}

		if (cgb::input().key_down(cgb::key_code::j)) {
			mLightDir = glm::vec4( glm::mat3(glm::rotate( cgb::time().delta_time(), glm::vec3{1.0f, 0.f, 0.f})) * glm::vec3(mLightDir), 0.0f);
		}
		if (cgb::input().key_down(cgb::key_code::l)) {
			mLightDir = glm::vec4( glm::mat3(glm::rotate(-cgb::time().delta_time(), glm::vec3{1.0f, 0.f, 0.f})) * glm::vec3(mLightDir), 0.0f);
		}
		if (cgb::input().key_down(cgb::key_code::i)) {
			mLightDir = glm::vec4( glm::mat3(glm::rotate( cgb::time().delta_time(), glm::vec3{0.0f, 0.f, 1.f})) * glm::vec3(mLightDir), 0.0f);
		}
		if (cgb::input().key_down(cgb::key_code::k)) {
			mLightDir = glm::vec4( glm::mat3(glm::rotate(-cgb::time().delta_time(), glm::vec3{0.0f, 0.f, 1.f})) * glm::vec3(mLightDir), 0.0f);
		}
		if (cgb::input().key_down(cgb::key_code::u)) {
			mLightDir = glm::vec4( glm::mat3(glm::rotate( cgb::time().delta_time(), glm::vec3{0.0f, 1.f, 0.f})) * glm::vec3(mLightDir), 0.0f);
		}
		if (cgb::input().key_down(cgb::key_code::o)) {
			mLightDir = glm::vec4( glm::mat3(glm::rotate(-cgb::time().delta_time(), glm::vec3{0.0f, 1.f, 0.f})) * glm::vec3(mLightDir), 0.0f);
		}
	}

	void render() override
	{
		auto inFlightIndex = cgb::context().main_window()->in_flight_index_for_frame();
		
		auto cmdbfr = cgb::context().graphics_queue().create_single_use_command_buffer();
		cmdbfr->begin_recording();
		cmdbfr->bind_pipeline(mPipeline);
		cmdbfr->bind_descriptors(mPipeline->layout(), { 
				cgb::binding(0, 0, mOffscreenImageViews[inFlightIndex]),
				cgb::binding(1, 0, mTLAS[inFlightIndex])
			}
		);

		// Set the push constants:
		auto pushConstantsForThisDrawCall = push_const_data { 
			mQuakeCam.view_matrix(),
			mLightDir
		};
		cmdbfr->handle().pushConstants(mPipeline->layout_handle(), vk::ShaderStageFlagBits::eRaygenNV | vk::ShaderStageFlagBits::eClosestHitNV, 0, sizeof(pushConstantsForThisDrawCall), &pushConstantsForThisDrawCall);

		// TRACE. THA. RAYZ.
		cmdbfr->handle().traceRaysNV(
			mPipeline->shader_binding_table_handle(), 0,
			mPipeline->shader_binding_table_handle(), 2 * mPipeline->table_entry_size(), mPipeline->table_entry_size(),
			mPipeline->shader_binding_table_handle(), 1 * mPipeline->table_entry_size(), mPipeline->table_entry_size(),
			nullptr, 0, 0,
			cgb::context().main_window()->swap_chain_extent().width, cgb::context().main_window()->swap_chain_extent().height, 1,
			cgb::context().dynamic_dispatch());

		cmdbfr->end_recording();
		submit_command_buffer_ownership(std::move(cmdbfr));
		present_image(mOffscreenImageViews[inFlightIndex]->get_image());
	}

private: // v== Member variables ==v
	std::chrono::high_resolution_clock::time_point mInitTime;

	// Multiple BLAS, concurrently used by all the (three?) TLASs:
	std::vector<cgb::bottom_level_acceleration_structure> mBLASs;
	// Geometry instance data which store the instance data per BLAS
	std::vector<cgb::geometry_instance> mGeometryInstances;
	// Multiple TLAS, one for each frame in flight:
	std::vector<cgb::top_level_acceleration_structure> mTLAS;

	std::vector<cgb::image_view> mOffscreenImageViews;

	glm::vec4 mLightDir;
	
	cgb::ray_tracing_pipeline mPipeline;
	cgb::quake_camera mQuakeCam;

}; // ray_tracing_basic_usage_app

int main() // <== Starting point ==
{
	try {
		// What's the name of our application
		cgb::settings::gApplicationName = "cg_base::ray_tracing_basic_usage";
		cgb::settings::gQueueSelectionPreference = cgb::device_queue_selection_strategy::prefer_everything_on_single_queue;
		cgb::settings::gRequiredDeviceExtensions.push_back(VK_NV_RAY_TRACING_EXTENSION_NAME);
		cgb::settings::gRequiredDeviceExtensions.push_back(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
		cgb::settings::gLoadImagesInSrgbFormatByDefault = true;

		// Create a window and open it
		auto mainWnd = cgb::context().create_window("cg_base: Real-Time Ray Tracing - Basic Usage Example");
		mainWnd->set_resolution({ 640, 480 });
		mainWnd->set_presentaton_mode(cgb::presentation_mode::vsync);
		mainWnd->set_additional_back_buffer_attachments({ cgb::attachment::create_depth(cgb::image_format::default_depth_format()) });
		mainWnd->open(); 

		// Create an instance of ray_tracing_triangle_meshes_app which, in this case,
		// contains the entire functionality of our application. 
		auto element = ray_tracing_basic_usage_app();

		// Create a composition of:
		//  - a timer
		//  - an executor
		//  - a behavior
		// ...
		auto hello = cgb::composition<cgb::varying_update_timer, cgb::sequential_executor>({
				&element
			});

		// ... and start that composition!
		hello.start();
	}
	catch (std::runtime_error& re)
	{
		LOG_ERROR_EM(re.what());
		//throw re;
	}
}


