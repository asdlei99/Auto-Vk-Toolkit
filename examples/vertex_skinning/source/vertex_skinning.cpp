#include <gvk.hpp>
#include <imgui.h>

class model_loader_app : public gvk::invokee {
  struct data_for_draw_call {
    std::vector<glm::vec3> mPositions;
    std::vector<glm::vec2> mTexCoords;
    std::vector<glm::vec3> mNormals;
    std::vector<uint32_t> mIndices;

    avk::buffer mPositionsBuffer;
    avk::buffer mTexCoordsBuffer;
    avk::buffer mNormalsBuffer;
    avk::buffer mIndexBuffer;

    int mMaterialIndex;
  };

  struct transformation_matrices {
    glm::mat4 mModelMatrix;
    int mMaterialIndex;
  };

public: // v== avk::invokee overrides which will be invoked by the framework ==v
  model_loader_app(avk::queue &aQueue)
      : mQueue{&aQueue}, mScale{1.0f, 1.0f, 1.0f} {}

  void initialize() override {
    mInitTime = std::chrono::high_resolution_clock::now();

    // Create a descriptor cache that helps us to conveniently create descriptor
    // sets:
    mDescriptorCache = gvk::context().create_descriptor_cache();

    // Load a model from file:
    auto animated_tube = gvk::model_t::load_from_file(
        "assets/skinning_test_tube_animation.fbx",
        aiProcess_Triangulate | aiProcess_PreTransformVertices
    );
    // Get all the different materials of the model:
    auto distinctMaterials = animated_tube->distinct_material_configs();

    // The following might be a bit tedious still, but maybe it's not. For what
    // it's worth, it is expressive. The following loop gathers all the vertex
    // and index data PER MATERIAL and constructs the buffers and materials.
    // Later, we'll use ONE draw call PER MATERIAL to draw the whole scene.
    std::vector<gvk::material_config> allMatConfigs;
    for (const auto &pair : distinctMaterials) {
      auto &newElement = mDrawCalls.emplace_back();
      allMatConfigs.push_back(pair.first);
      newElement.mMaterialIndex = static_cast<int>(allMatConfigs.size() - 1);

      // 1. Gather all the vertex and index data from the sub meshes:
      for (auto index : pair.second) {
        gvk::append_indices_and_vertex_data(
            gvk::additional_index_data(
                newElement.mIndices,
                [&]() {
                  return animated_tube->indices_for_mesh<uint32_t>(index);
                }
            ),
            gvk::additional_vertex_data(
                newElement.mPositions,
                [&]() { return animated_tube->positions_for_mesh(index); }
            ),
            gvk::additional_vertex_data(
                newElement.mTexCoords,
                [&]() {
                  return animated_tube->texture_coordinates_for_mesh<glm::vec2>(
                      index, 0);
                }
            ),
            gvk::additional_vertex_data(newElement.mNormals, [&]() {
              return animated_tube->normals_for_mesh(index);
            })
        );
      }

      // 2. Build all the buffers for the GPU
      // 2.1 Positions:
      newElement.mPositionsBuffer = gvk::context().create_buffer(
          avk::memory_usage::device, {},
          avk::vertex_buffer_meta::create_from_data(newElement.mPositions)
      );
      newElement.mPositionsBuffer->fill(
          newElement.mPositions.data(), 0,
          avk::sync::with_barriers(
              gvk::context().main_window()->command_buffer_lifetime_handler()
          )
      );
      // 2.2 Texture Coordinates:
      newElement.mTexCoordsBuffer = gvk::context().create_buffer(
          avk::memory_usage::device, {},
          avk::vertex_buffer_meta::create_from_data(newElement.mTexCoords)
      );
      newElement.mTexCoordsBuffer->fill(
          newElement.mTexCoords.data(), 0,
          avk::sync::with_barriers(
              gvk::context().main_window()->command_buffer_lifetime_handler()
          )
      );
      // 2.3 Normals:
      newElement.mNormalsBuffer = gvk::context().create_buffer(
          avk::memory_usage::device, {},
          avk::vertex_buffer_meta::create_from_data(newElement.mNormals)
      );
      newElement.mNormalsBuffer->fill(
          newElement.mNormals.data(), 0,
          avk::sync::with_barriers(
              gvk::context().main_window()->command_buffer_lifetime_handler()
          )
      );
      // 2.4 Indices:
      newElement.mIndexBuffer = gvk::context().create_buffer(
          avk::memory_usage::device, {},
          avk::index_buffer_meta::create_from_data(newElement.mIndices)
      );
      newElement.mIndexBuffer->fill(
          newElement.mIndices.data(), 0,
          avk::sync::with_barriers(
              gvk::context().main_window()->command_buffer_lifetime_handler()
          )
      );
    }

    // For all the different materials, transfer them in structs which are well
    // suited for GPU-usage (proper alignment, and containing only the relevant
    // data), also load all the referenced images from file and provide access
    // to them via samplers; It all happens in `ak::convert_for_gpu_usage`:
    auto [gpuMaterials, imageSamplers] = gvk::convert_for_gpu_usage<gvk::material_gpu_data>(
          allMatConfigs,
          false,
          true,
          avk::image_usage::general_texture,
          avk::filter_mode::trilinear,
          avk::sync::with_barriers(
              gvk::context().main_window()->command_buffer_lifetime_handler()
          )
    );

    mViewProjBuffer = gvk::context().create_buffer(
        avk::memory_usage::host_visible, {},
        avk::uniform_buffer_meta::create_from_data(glm::mat4())
    );

    mMaterialBuffer = gvk::context().create_buffer(
        avk::memory_usage::host_visible, {},
        avk::storage_buffer_meta::create_from_data(gpuMaterials)
    );
    mMaterialBuffer->fill(gpuMaterials.data(), 0, avk::sync::not_required());

    mImageSamplers = std::move(imageSamplers);

    auto swapChainFormat = gvk::context().main_window()->swap_chain_image_format();
    // Create our rasterization graphics pipeline with the required
    // configuration:
    mPipeline = gvk::context().create_graphics_pipeline_for(
        // Specify which shaders the pipeline consists of:
        avk::vertex_shader("shaders/transform_and_pass_pos_nrm_uv.vert"),
        avk::fragment_shader("shaders/diffuse_shading_fixed_lightsource.frag"),
        // The next 3 lines define the format and location of the vertex shader
        // inputs: (The dummy values (like glm::vec3) tell the pipeline the
        // format of the respective input)
        avk::from_buffer_binding(0)
            ->stream_per_vertex<glm::vec3>()
            ->to_location(0), // <-- corresponds to vertex shader's inPosition
        avk::from_buffer_binding(1)
            ->stream_per_vertex<glm::vec2>()
            ->to_location(1), // <-- corresponds to vertex shader's inTexCoord
        avk::from_buffer_binding(2)
            ->stream_per_vertex<glm::vec3>()
            ->to_location(2), // <-- corresponds to vertex shader's inNormal
        // Some further settings:
        avk::cfg::front_face::define_front_faces_to_be_counter_clockwise(),
        avk::cfg::viewport_depth_scissors_config::from_framebuffer(
            gvk::context().main_window()->backbuffer_at_index(0)
        ),
        // We'll render to the back buffer, which has a color attachment always,
        // and in our case additionally a depth attachment, which has been
        // configured when creating the window. See main() function!
        avk::attachment::declare(
            gvk::format_from_window_color_buffer(gvk::context().main_window()),
            avk::on_load::clear, avk::color(0), avk::on_store::store
        ),
        // But not in presentable format, because ImGui comes after
        avk::attachment::declare(
            gvk::format_from_window_depth_buffer(gvk::context().main_window()),
            avk::on_load::clear, avk::depth_stencil(),
            avk::on_store::dont_care
        ),
        // The following define additional data which we'll pass to the
        // pipeline: We'll pass two matrices to our vertex shader via push
        // constants:
        avk::push_constant_binding_data{
            avk::shader_type::vertex, 0,
            sizeof(transformation_matrices)
        },
        avk::descriptor_binding(0, 0, mImageSamplers),
        avk::descriptor_binding(0, 1, mViewProjBuffer),
        avk::descriptor_binding(1, 0, mMaterialBuffer)
    );

    // set up updater
    // we want to use an updater, so create one:

    mUpdater.emplace();
    mPipeline.enable_shared_ownership(); // Make it usable with the updater

    mUpdater->on(gvk::swapchain_resized_event(gvk::context().main_window()))
        .invoke([this]() {
          this->mQuakeCam.set_aspect_ratio(
              gvk::context().main_window()->aspect_ratio());
        });

    // first make sure render pass is updated
    mUpdater->on(
        gvk::swapchain_format_changed_event(gvk::context().main_window()),
        gvk::swapchain_additional_attachments_changed_event(
            gvk::context().main_window()
        )
    ).invoke([this]() {
        std::vector<avk::attachment> renderpassAttachments = {
            avk::attachment::declare(
                gvk::format_from_window_color_buffer(
                    gvk::context().main_window()
                ),
                avk::on_load::clear, avk::color(0),
                avk::on_store::store
            ), // But not in presentable format,
            // because ImGui comes after
        };
        auto renderPass =
            gvk::context().create_renderpass(renderpassAttachments);
        gvk::context().replace_render_pass_for_pipeline(
            mPipeline, std::move(renderPass)
        );
    }).then_on( // ... next, at this point, we are sure that the render pass
                // is correct -> check if there are events that would update
                // the pipeline
        gvk::swapchain_changed_event(gvk::context().main_window()),
        gvk::shader_files_changed_event(mPipeline)
    ).update(mPipeline);

    // Add the camera to the composition (and let it handle the updates)
    mQuakeCam.set_translation({0.0f, 1.5f, 4.0f});
    mQuakeCam.look_at(glm::vec3(0.0f, 1.5f, 0.0f));
    mQuakeCam.set_perspective_projection(
        glm::radians(60.0f), gvk::context().main_window()->aspect_ratio(), 0.3f,
        1000.0f
    );
    gvk::current_composition()->add_element(mQuakeCam);
  }

  void render() override {
    auto mainWnd = gvk::context().main_window();

    auto viewProjMat = mQuakeCam.projection_matrix() * mQuakeCam.view_matrix();
    mViewProjBuffer->fill(
        glm::value_ptr(viewProjMat), 0, avk::sync::not_required()
    );

    auto &commandPool =
        gvk::context().get_command_pool_for_single_use_command_buffers(*mQueue);
    auto cmdbfr = commandPool->alloc_command_buffer(
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit
    );
    cmdbfr->begin_recording();
    cmdbfr->begin_render_pass_for_framebuffer(
        mPipeline->get_renderpass(),
        gvk::context().main_window()->current_backbuffer()
    );
    cmdbfr->bind_pipeline(avk::const_referenced(mPipeline));
    cmdbfr->bind_descriptors(
        mPipeline->layout(),
        mDescriptorCache.get_or_create_descriptor_sets({
            avk::descriptor_binding(0, 0, mImageSamplers),
            avk::descriptor_binding(0, 1, mViewProjBuffer),
            avk::descriptor_binding(1, 0, mMaterialBuffer)
        })
    );

    for (auto &drawCall : mDrawCalls) {
      // Set the push constants:
      auto pushConstantsForThisDrawCall = transformation_matrices {
          // Set model matrix for this mesh:
          glm::scale(glm::vec3(0.01f) * mScale),
          // Set material index for this mesh:
          drawCall.mMaterialIndex
      };
      cmdbfr->handle().pushConstants(
          mPipeline->layout_handle(), vk::ShaderStageFlagBits::eVertex, 0,
          sizeof(pushConstantsForThisDrawCall), &pushConstantsForThisDrawCall
      );

      // Make the draw call:
      cmdbfr->draw_indexed(
          // Bind and use the index buffer:
          avk::const_referenced(drawCall.mIndexBuffer),
          // Bind the vertex input buffers in the right order (corresponding to
          // the layout specifiers in the vertex shader)
          avk::const_referenced(drawCall.mPositionsBuffer),
          avk::const_referenced(drawCall.mTexCoordsBuffer),
          avk::const_referenced(drawCall.mNormalsBuffer)
      );
    }

    cmdbfr->end_render_pass();
    cmdbfr->end_recording();

    // The swap chain provides us with an "image available semaphore" for the
    // current frame. Only after the swapchain image has become available, we
    // may start rendering into it.
    auto imageAvailableSemaphore =
        mainWnd->consume_current_image_available_semaphore();

    // Submit the draw call and take care of the command buffer's lifetime:
    mQueue->submit(cmdbfr, imageAvailableSemaphore);
    mainWnd->handle_lifetime(avk::owned(cmdbfr));
  }

  void update() override {
    static int counter = 0;
    if (++counter == 4) {
      auto current = std::chrono::high_resolution_clock::now();
      auto time_span = current - mInitTime;
      auto int_min =
          std::chrono::duration_cast<std::chrono::minutes>(time_span).count();
      auto int_sec =
          std::chrono::duration_cast<std::chrono::seconds>(time_span).count();
      auto fp_ms = std::chrono::duration<double, std::milli>(time_span).count();
      printf("Time from init to fourth frame: %d min, %lld sec %lf ms\n",
           int_min, int_sec - static_cast<decltype(int_sec)>(int_min) * 60,
           fp_ms - 1000.0 * int_sec
      );
    }

    if (gvk::input().key_pressed(gvk::key_code::c)) {
      // Center the cursor:
      auto resolution = gvk::context().main_window()->resolution();
      gvk::context().main_window()->set_cursor_pos(
          {resolution[0] / 2.0, resolution[1] / 2.0}
      );
    }
    if (gvk::input().key_pressed(gvk::key_code::escape)) {
      // Stop the current composition:
      gvk::current_composition()->stop();
    }
    if (gvk::input().key_pressed(gvk::key_code::left)) {
      mQuakeCam.look_along(gvk::left());
    }
    if (gvk::input().key_pressed(gvk::key_code::right)) {
      mQuakeCam.look_along(gvk::right());
    }
    if (gvk::input().key_pressed(gvk::key_code::up)) {
      mQuakeCam.look_along(gvk::front());
    }
    if (gvk::input().key_pressed(gvk::key_code::down)) {
      mQuakeCam.look_along(gvk::back());
    }
    if (gvk::input().key_pressed(gvk::key_code::page_up)) {
      mQuakeCam.look_along(gvk::up());
    }
    if (gvk::input().key_pressed(gvk::key_code::page_down)) {
      mQuakeCam.look_along(gvk::down());
    }
    if (gvk::input().key_pressed(gvk::key_code::home)) {
      mQuakeCam.look_at(glm::vec3{0.0f, 0.0f, 0.0f});
    }

    if (gvk::input().key_pressed(gvk::key_code::f1)) {
      auto imguiManager =
          gvk::current_composition()->element_by_type<gvk::imgui_manager>();
      if (mQuakeCam.is_enabled()) {
        mQuakeCam.disable();
        if (nullptr != imguiManager) {
          imguiManager->enable_user_interaction(true);
        }
      } else {
        mQuakeCam.enable();
        if (nullptr != imguiManager) {
          imguiManager->enable_user_interaction(false);
        }
      }
    }
  }

private: // v== Member variables ==v
  std::chrono::high_resolution_clock::time_point mInitTime;

  avk::queue *mQueue;
  avk::descriptor_cache mDescriptorCache;

  avk::buffer mViewProjBuffer;
  avk::buffer mMaterialBuffer;
  std::vector<avk::image_sampler> mImageSamplers;

  std::vector<data_for_draw_call> mDrawCalls;
  avk::graphics_pipeline mPipeline;
  gvk::quake_camera mQuakeCam;

  glm::vec3 mScale;
}; // model_loader_app

int main() // <== Starting point ==
{
  try {
    // Create a window and open it
    auto mainWnd = gvk::context().create_window("Vertex Skinning");

    mainWnd->set_resolution({1000, 480});
    mainWnd->enable_resizing(true);
    mainWnd->set_additional_back_buffer_attachments({
        avk::attachment::declare(
            vk::Format::eD32Sfloat, avk::on_load::clear, avk::depth_stencil(),
            avk::on_store::dont_care
        )
    });
    mainWnd->set_presentaton_mode(gvk::presentation_mode::mailbox);
    mainWnd->set_number_of_concurrent_frames(3u);
    mainWnd->open();

    auto &singleQueue = gvk::context().create_queue(
        {}, avk::queue_selection_preference::versatile_queue, mainWnd
    );
    mainWnd->add_queue_family_ownership(singleQueue);
    mainWnd->set_present_queue(singleQueue);

    // Create an instance of our main avk::element which contains all the
    // functionality:
    auto app = model_loader_app(singleQueue);
    // Create another element for drawing the UI with ImGui
    auto ui = gvk::imgui_manager(singleQueue);

    // GO:
    gvk::start(
        gvk::application_name("Gears-Vk + Auto-Vk Example: Model Loader"),
        mainWnd, app, ui
    );
  } catch (gvk::logic_error &) {
  } catch (gvk::runtime_error &) {
  } catch (avk::logic_error &) {
  } catch (avk::runtime_error &) {
  }
}