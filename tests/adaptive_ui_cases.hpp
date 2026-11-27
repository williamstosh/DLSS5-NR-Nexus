// CPU ReShade interfaces for owner matching. The DEV fixture includes only these
// declarations with NR_NEXUS_TEST_OUTPUT_RUNTIME_ONLY.
namespace fixture_rate = renodx::addons::dlss5::output_framerate;
namespace fixture_runtime = renodx::addons::dlss5::internal;
struct OutputRuntimeFixture final : reshade::api::effect_runtime {
  struct Device final : reshade::api::device, IUnknown {
    reshade::api::resource_desc description{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
      if (output == nullptr) return E_POINTER;
      *output = nullptr;
      if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
      *output = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(static_cast<const IUnknown*>(this)); }
    void get_private_data(const uint8_t[16], uint64_t*) const override { throw std::logic_error("unsupported output fixture operation"); }
    void set_private_data(const uint8_t[16], uint64_t) override { throw std::logic_error("unsupported output fixture operation"); }
    reshade::api::device_api get_api() const override { return reshade::api::device_api::d3d12; }
    bool check_capability(reshade::api::device_caps capability) const override { throw std::logic_error("unsupported output fixture operation"); }
    bool check_format_support(reshade::api::format format, reshade::api::resource_usage usage) const override { throw std::logic_error("unsupported output fixture operation"); }
    bool create_sampler(const reshade::api::sampler_desc &desc, reshade::api::sampler *out_sampler) override { throw std::logic_error("unsupported output fixture operation"); }
    void destroy_sampler(reshade::api::sampler sampler) override { throw std::logic_error("unsupported output fixture operation"); }
    bool create_resource(const reshade::api::resource_desc &desc, const reshade::api::subresource_data *initial_data, reshade::api::resource_usage initial_state, reshade::api::resource *out_resource, void **shared_handle = nullptr) override { throw std::logic_error("unsupported output fixture operation"); }
    void destroy_resource(reshade::api::resource resource) override { throw std::logic_error("unsupported output fixture operation"); }
    reshade::api::resource_desc get_resource_desc(reshade::api::resource resource) const override { return description; }
    bool create_resource_view(reshade::api::resource resource, reshade::api::resource_usage usage_type, const reshade::api::resource_view_desc &desc, reshade::api::resource_view *out_view) override { throw std::logic_error("unsupported output fixture operation"); }
    void destroy_resource_view(reshade::api::resource_view view) override { throw std::logic_error("unsupported output fixture operation"); }
    reshade::api::resource get_resource_from_view(reshade::api::resource_view view) const override { throw std::logic_error("unsupported output fixture operation"); }
    reshade::api::resource_view_desc get_resource_view_desc(reshade::api::resource_view view) const override { throw std::logic_error("unsupported output fixture operation"); }
    bool map_buffer_region(reshade::api::resource resource, uint64_t offset, uint64_t size, reshade::api::map_access access, void **out_data) override { throw std::logic_error("unsupported output fixture operation"); }
    void unmap_buffer_region(reshade::api::resource resource) override { throw std::logic_error("unsupported output fixture operation"); }
    bool map_texture_region(reshade::api::resource resource, uint32_t subresource, const reshade::api::subresource_box *box, reshade::api::map_access access, reshade::api::subresource_data *out_data) override { throw std::logic_error("unsupported output fixture operation"); }
    void unmap_texture_region(reshade::api::resource resource, uint32_t subresource) override { throw std::logic_error("unsupported output fixture operation"); }
    void update_buffer_region(const void *data, reshade::api::resource dest, uint64_t dest_offset, uint64_t size) override { throw std::logic_error("unsupported output fixture operation"); }
    void update_texture_region(const reshade::api::subresource_data &data, reshade::api::resource dest, uint32_t dest_subresource, const reshade::api::subresource_box *dest_box = nullptr) override { throw std::logic_error("unsupported output fixture operation"); }
    bool create_pipeline(reshade::api::pipeline_layout layout, uint32_t subobject_count, const reshade::api::pipeline_subobject *subobjects, reshade::api::pipeline *out_pipeline) override { throw std::logic_error("unsupported output fixture operation"); }
    void destroy_pipeline(reshade::api::pipeline pipeline) override { throw std::logic_error("unsupported output fixture operation"); }
    bool create_pipeline_layout(uint32_t param_count, const reshade::api::pipeline_layout_param *params, reshade::api::pipeline_layout *out_layout) override { throw std::logic_error("unsupported output fixture operation"); }
    void destroy_pipeline_layout(reshade::api::pipeline_layout layout) override { throw std::logic_error("unsupported output fixture operation"); }
    bool allocate_descriptor_tables(uint32_t count, reshade::api::pipeline_layout layout, uint32_t param, reshade::api::descriptor_table *out_tables) override { throw std::logic_error("unsupported output fixture operation"); }
    void free_descriptor_tables(uint32_t count, const reshade::api::descriptor_table *tables) override { throw std::logic_error("unsupported output fixture operation"); }
    void get_descriptor_heap_offset(reshade::api::descriptor_table table, uint32_t binding, uint32_t array_offset, reshade::api::descriptor_heap *out_heap, uint32_t *out_offset) const override { throw std::logic_error("unsupported output fixture operation"); }
    void copy_descriptor_tables(uint32_t count, const reshade::api::descriptor_table_copy *copies) override { throw std::logic_error("unsupported output fixture operation"); }
    void update_descriptor_tables(uint32_t count, const reshade::api::descriptor_table_update *updates) override { throw std::logic_error("unsupported output fixture operation"); }
    bool create_query_heap(reshade::api::query_type type, uint32_t count, reshade::api::query_heap *out_heap) override { throw std::logic_error("unsupported output fixture operation"); }
    void destroy_query_heap(reshade::api::query_heap heap) override { throw std::logic_error("unsupported output fixture operation"); }
    bool get_query_heap_results(reshade::api::query_heap heap, uint32_t first, uint32_t count, void *results, uint32_t stride) override { throw std::logic_error("unsupported output fixture operation"); }
    void set_resource_name(reshade::api::resource resource, const char *name) override { throw std::logic_error("unsupported output fixture operation"); }
    void set_resource_view_name(reshade::api::resource_view view, const char *name) override { throw std::logic_error("unsupported output fixture operation"); }
    bool create_fence(uint64_t initial_value, reshade::api::fence_flags flags, reshade::api::fence *out_fence, void **shared_handle = nullptr) override { throw std::logic_error("unsupported output fixture operation"); }
    void destroy_fence(reshade::api::fence fence) override { throw std::logic_error("unsupported output fixture operation"); }
    uint64_t get_completed_fence_value(reshade::api::fence fence) const override { throw std::logic_error("unsupported output fixture operation"); }
    bool wait(reshade::api::fence fence, uint64_t value, uint64_t timeout = UINT64_MAX) override { throw std::logic_error("unsupported output fixture operation"); }
    bool signal(reshade::api::fence fence, uint64_t value) override { throw std::logic_error("unsupported output fixture operation"); }
    bool get_property(reshade::api::device_properties property, void *data) const override { throw std::logic_error("unsupported output fixture operation"); }
    uint64_t get_resource_view_gpu_address(reshade::api::resource_view view) const override { throw std::logic_error("unsupported output fixture operation"); }
    void get_acceleration_structure_size(reshade::api::acceleration_structure_type type, reshade::api::acceleration_structure_build_flags flags, uint32_t input_count, const reshade::api::acceleration_structure_build_input *inputs, uint64_t *out_size, uint64_t *out_build_scratch_size, uint64_t *out_update_scratch_size) const override { throw std::logic_error("unsupported output fixture operation"); }
    bool get_pipeline_shader_group_handles(reshade::api::pipeline pipeline, uint32_t first, uint32_t count, void *out_handles) override { throw std::logic_error("unsupported output fixture operation"); }
  } device;
  std::array<reshade::api::resource, 2> buffers{{{0xADA100}, {0xADA200}}};
  uint32_t current_buffer = 0;
  fixture_rate::OutputIdentity output;
  reshade::api::effect_runtime* saved_owner = fixture_runtime::monitor_runtime.load();
  fixture_runtime::MonitorCadence saved_cadence = fixture_runtime::monitor_cadence;
  fixture_runtime::MonitorSnapshot saved_snapshot = fixture_runtime::monitor_snapshot;
  decltype(fixture_rate::internal::state.present) saved_present;
  decltype(fixture_rate::internal::state.xefg) saved_xefg;
  decltype(fixture_rate::internal::state.native_options) saved_options;

  OutputRuntimeFixture() {
    {
      std::unique_lock lock(fixture_rate::internal::state.mutex);
      saved_present.swap(fixture_rate::internal::state.present);
      saved_xefg.swap(fixture_rate::internal::state.xefg);
      saved_options.swap(fixture_rate::internal::state.native_options);
    }
    device.description.texture.width = 3840;
    device.description.texture.height = 2160;
    const std::array<uintptr_t, 2> back_buffers{buffers[0].handle, buffers[1].handle};
    fixture_rate::RegisterPresentOutput(reinterpret_cast<uintptr_t>(this), device.get_native(),
        reinterpret_cast<uintptr_t>(this), back_buffers);
    output = fixture_runtime::MatchRuntimeOutput(this);
    fixture_runtime::monitor_runtime = this;
    fixture_runtime::monitor_cadence = {.owner = this, .output = output,
        .epoch = fixture_runtime::monitor_history_epoch.load()};
    fixture_runtime::monitor_snapshot = {};
  }

  ~OutputRuntimeFixture() {
    fixture_runtime::monitor_runtime = saved_owner;
    fixture_runtime::monitor_cadence = std::move(saved_cadence);
    fixture_runtime::monitor_snapshot = std::move(saved_snapshot);
    std::unique_lock lock(fixture_rate::internal::state.mutex);
    saved_present.swap(fixture_rate::internal::state.present);
    saved_xefg.swap(fixture_rate::internal::state.xefg);
    saved_options.swap(fixture_rate::internal::state.native_options);
  }
  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(this); }
  void get_private_data(const uint8_t guid[16], uint64_t *data) const override { throw std::logic_error("unsupported output fixture operation"); }
  void set_private_data(const uint8_t guid[16], const uint64_t data) override { throw std::logic_error("unsupported output fixture operation"); }
  reshade::api::device *get_device() override { return &device; }
  void *get_hwnd() const override { return nullptr; }
  reshade::api::resource get_back_buffer(uint32_t index) override { return buffers.at(index); }
  uint32_t get_back_buffer_count() const override { return static_cast<uint32_t>(buffers.size()); }
  uint32_t get_current_back_buffer_index() const override { return current_buffer; }
  reshade::api::command_queue *get_command_queue() override { return nullptr; }
  void render_effects(reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) override { throw std::logic_error("unsupported output fixture operation"); }
  bool capture_screenshot(void *pixels) override { throw std::logic_error("unsupported output fixture operation"); }
  void get_screenshot_width_and_height(uint32_t *out_width, uint32_t *out_height) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool is_key_down(uint32_t keycode) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool is_key_pressed(uint32_t keycode) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool is_key_released(uint32_t keycode) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool is_mouse_button_down(uint32_t button) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool is_mouse_button_pressed(uint32_t button) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool is_mouse_button_released(uint32_t button) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_mouse_cursor_position(uint32_t *out_x, uint32_t *out_y, int16_t *out_wheel_delta = nullptr) const override { throw std::logic_error("unsupported output fixture operation"); }
  void enumerate_uniform_variables(const char *effect_name, void(*callback)(reshade::api::effect_runtime *runtime, reshade::api::effect_uniform_variable variable, void *user_data), void *user_data) override { throw std::logic_error("unsupported output fixture operation"); }
  reshade::api::effect_uniform_variable find_uniform_variable(const char *effect_name, const char *variable_name) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_uniform_variable_type(reshade::api::effect_uniform_variable variable, reshade::api::format *out_base_type, uint32_t *out_rows = nullptr, uint32_t *out_columns = nullptr, uint32_t *out_array_length = nullptr) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_uniform_variable_name(reshade::api::effect_uniform_variable variable, char *name, size_t *name_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_bool_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char *name, bool *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_float_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char *name, float *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_int_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char *name, int32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_uint_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char *name, uint32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_string_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char *name, char *value, size_t *value_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_uniform_value_bool(reshade::api::effect_uniform_variable variable, bool *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_uniform_value_float(reshade::api::effect_uniform_variable variable, float *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_uniform_value_int(reshade::api::effect_uniform_variable variable, int32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_uniform_value_uint(reshade::api::effect_uniform_variable variable, uint32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  void set_uniform_value_bool(reshade::api::effect_uniform_variable variable, const bool *values, size_t count, size_t array_index = 0) override { throw std::logic_error("unsupported output fixture operation"); }
  void set_uniform_value_float(reshade::api::effect_uniform_variable variable, const float *values, size_t count, size_t array_index = 0) override { throw std::logic_error("unsupported output fixture operation"); }
  void set_uniform_value_int(reshade::api::effect_uniform_variable variable, const int32_t *values, size_t count, size_t array_index = 0) override { throw std::logic_error("unsupported output fixture operation"); }
  void set_uniform_value_uint(reshade::api::effect_uniform_variable variable, const uint32_t *values, size_t count, size_t array_index = 0) override { throw std::logic_error("unsupported output fixture operation"); }
  void enumerate_texture_variables(const char *effect_name, void(*callback)(reshade::api::effect_runtime *runtime, reshade::api::effect_texture_variable variable, void *user_data), void *user_data) override { throw std::logic_error("unsupported output fixture operation"); }
  reshade::api::effect_texture_variable find_texture_variable(const char *effect_name, const char *variable_name) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_texture_variable_name(reshade::api::effect_texture_variable variable, char *name, size_t *name_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_bool_from_texture_variable(reshade::api::effect_texture_variable variable, const char *name, bool *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_float_from_texture_variable(reshade::api::effect_texture_variable variable, const char *name, float *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_int_from_texture_variable(reshade::api::effect_texture_variable variable, const char *name, int32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_uint_from_texture_variable(reshade::api::effect_texture_variable variable, const char *name, uint32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_string_from_texture_variable(reshade::api::effect_texture_variable variable, const char *name, char *value, size_t *value_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void update_texture(reshade::api::effect_texture_variable variable, const uint32_t width, const uint32_t height, const void *pixels) override { throw std::logic_error("unsupported output fixture operation"); }
  void get_texture_binding(reshade::api::effect_texture_variable variable, reshade::api::resource_view *out_srv, reshade::api::resource_view *out_srv_srgb) const override { throw std::logic_error("unsupported output fixture operation"); }
  void update_texture_bindings(const char *semantic, reshade::api::resource_view srv, reshade::api::resource_view srv_srgb) override { throw std::logic_error("unsupported output fixture operation"); }
  void enumerate_techniques(const char *effect_name, void(*callback)(reshade::api::effect_runtime *runtime, reshade::api::effect_technique technique, void *user_data), void *user_data) override { throw std::logic_error("unsupported output fixture operation"); }
  reshade::api::effect_technique find_technique(const char *effect_name, const char *technique_name) override { throw std::logic_error("unsupported output fixture operation"); }
  void get_technique_name(reshade::api::effect_technique technique, char *name, size_t *name_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_bool_from_technique(reshade::api::effect_technique technique, const char *name, bool *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_float_from_technique(reshade::api::effect_technique technique, const char *name, float *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_int_from_technique(reshade::api::effect_technique technique, const char *name, int32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_uint_from_technique(reshade::api::effect_technique technique, const char *name, uint32_t *values, size_t count, size_t array_index = 0) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_annotation_string_from_technique(reshade::api::effect_technique technique, const char *name, char *value, size_t *value_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_technique_state(reshade::api::effect_technique technique) const override { throw std::logic_error("unsupported output fixture operation"); }
  void set_technique_state(reshade::api::effect_technique technique, bool enabled) override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_preprocessor_definition(const char *name, char *value, size_t *value_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void set_preprocessor_definition(const char *name, const char *value) override { throw std::logic_error("unsupported output fixture operation"); }
  void render_technique(reshade::api::effect_technique technique, reshade::api::command_list *cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb = { 0 }) override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_effects_state() const override { throw std::logic_error("unsupported output fixture operation"); }
  void set_effects_state(bool enabled) override { throw std::logic_error("unsupported output fixture operation"); }
  void get_current_preset_path(char *path, size_t *path_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void set_current_preset_path(const char *path) override { throw std::logic_error("unsupported output fixture operation"); }
  void reorder_techniques(size_t count, const reshade::api::effect_technique *techniques) override { throw std::logic_error("unsupported output fixture operation"); }
  void block_input_next_frame() override { throw std::logic_error("unsupported output fixture operation"); }
  uint32_t last_key_pressed() const override { throw std::logic_error("unsupported output fixture operation"); }
  uint32_t last_key_released() const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_uniform_variable_effect_name(reshade::api::effect_uniform_variable variable, char *effect_name, size_t *effect_name_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_texture_variable_effect_name(reshade::api::effect_texture_variable variable, char *effect_name, size_t *effect_name_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void get_technique_effect_name(reshade::api::effect_technique technique, char *effect_name, size_t *effect_name_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void save_current_preset() const override { throw std::logic_error("unsupported output fixture operation"); }
  bool get_preprocessor_definition_for_effect(const char *effect_name, const char *name, char *value, size_t *value_size) const override { throw std::logic_error("unsupported output fixture operation"); }
  void set_preprocessor_definition_for_effect(const char *effect_name, const char *name, const char *value) override { throw std::logic_error("unsupported output fixture operation"); }
  bool open_overlay(bool open, reshade::api::input_source source) override { throw std::logic_error("unsupported output fixture operation"); }
  void set_color_space(reshade::api::color_space color_space) override { throw std::logic_error("unsupported output fixture operation"); }
  void reset_uniform_value(reshade::api::effect_uniform_variable variable) override { throw std::logic_error("unsupported output fixture operation"); }
  void reload_effect_next_frame(const char *effect_name) override { throw std::logic_error("unsupported output fixture operation"); }
  void export_current_preset(const char *path) const override { throw std::logic_error("unsupported output fixture operation"); }
  void save_screenshot(const char *postfix = nullptr) override { throw std::logic_error("unsupported output fixture operation"); }
};

#if !defined(NR_NEXUS_TEST_OUTPUT_RUNTIME_ONLY)
{
  OutputRuntimeFixture output_runtime;
  check(static_cast<bool>(output_runtime.output), "the CPU runtime resolves its registered current backbuffer");
  const bool saved_edit_reuse = runtime::edit_reuse_enabled.exchange(false);
  namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
  using DisplayStage = renodx::addons::dlss5::stage_plan::Stage;
  constexpr uint64_t display_source_generation = 73;
  const auto publish_display_status = [&](adaptive::Status status, DisplayStage stage, uint64_t source_generation) {
    status.display_output_generation = runtime::MatchRuntimeOutput(&output_runtime).generation;
    status.display_source_generation = source_generation;
    status.display_stage = static_cast<uint32_t>(stage);
    runtime::PublishAdaptiveStatus(status);
  };
  const auto refresh_monitor = [&] {
    runtime::monitor_cadence.frame = -1;
    runtime::UpdateMonitorCadence(&output_runtime);
    runtime::monitor_snapshot.attempted_ns = 0;
    return runtime::RefreshMonitorSnapshot(true);
  };
  namespace fixture_timers = renodx::addons::dlss5::gpu_timers;
  const auto saved_completion_observer = std::exchange(fixture_timers::completed_observer, &runtime::ObserveAdaptiveCompletion);
  const auto publish_completion = [](const fixture_timers::Stats& sample) {
    renodx::addons::dlss5::RuntimeLock lock(runtime::runtime_mutex);
    const auto stage = fixture_timers::StageIndex(sample.path);
    runtime::TouchAdaptiveCompletionScope(stage, {sample.identity.display_output_generation, sample.identity.source_generation},
        sample.recorded_ns);
    fixture_timers::internal::PublishCompleted(sample, 0, 1);
  };
  runtime::ui_language_choice = 1;
  ui::SelectLanguage(0);
  panel_height = 1600.f;
  panel_width = 1000.f;
  runtime::nr_follow_input_res = false;
  runtime::nr_resolution_scale = .75f;
  runtime::nr_adaptive_enabled = true;
  runtime::nr_adaptive_limited_residency = false;
  ui::MonitorInputs actual;
  actual.pass_count = 2;
  actual.passes[0].execution_confirmed = true;
  actual.passes[0].actual_size = "1600 x 900 (63%)";
  actual.passes[0].actual_scale = .625f;
  actual.passes[1].execution_confirmed = true;
  actual.passes[1].actual_size = "1280 x 720 (50%)";
  actual.passes[1].actual_scale = .5f;
  resolution_observed_fixture = &actual;
  adaptive_editor_fixture = true;
  frame(); frame();
  renodx::addons::dlss5::gpu_comparison::Identity preparation_before, allocation_before;
  runtime::comparison_ui::CaptureSettings(&preparation_before);
  runtime::comparison_ui::CaptureAdaptiveSettings(&allocation_before);
  const auto preparation_generation = runtime::configuration_generation.load();
  const auto preparation_level_count = runtime::nr_adaptive_selected_levels.load();
  click(find("On-demand (experimental)"));
  bool stored_limited = false;
  reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetAdaptiveLimitedResidency.key, stored_limited);
  renodx::addons::dlss5::gpu_comparison::Identity preparation_after, allocation_after;
  runtime::comparison_ui::CaptureSettings(&preparation_after);
  runtime::comparison_ui::CaptureAdaptiveSettings(&allocation_after);
  check(runtime::nr_adaptive_limited_residency.load() && stored_limited
      && runtime::nr_adaptive_selected_levels.load() == preparation_level_count
      && preparation_after.config_epoch != preparation_before.config_epoch
      && preparation_after.settings_hash != preparation_before.settings_hash
      && allocation_after.settings_hash == allocation_before.settings_hash
      && runtime::configuration_generation == preparation_generation,
      "the real preparation choice commits and persists without reducing levels or recreating compatible resources");
  click(find("Prepare all levels"));
  reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetAdaptiveLimitedResidency.key, stored_limited);
  check(!runtime::nr_adaptive_limited_residency.load() && !stored_limited,
      "the real full-preparation choice restores the persisted default residency policy");
  check(find("##actual_resolution_scale").disabled, "adaptive actual scale cannot be edited");
  check(std::none_of(items.begin(), items.end(), [](const auto& entry) {
    return entry.second.label == "##resolution_scale" || entry.second.label.ends_with("###nr_resolution_quality")
        || entry.second.label == "resolution_reset" || entry.second.label == "resolution_scale_reset";
  }), "adaptive mode exposes no manual resolution preset, slider or reset");
  const auto epoch = runtime::profile_commit_epoch.load();
  click(find("##actual_resolution_scale"));
  check(runtime::nr_resolution_scale.load() == .75f && runtime::profile_commit_epoch.load() == epoch,
        "clicking the actual scale preserves the saved manual upper limit");
  click(find("##adaptive_target_exact"));
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(ImGuiKey_A, true);
  frame();
  io.AddKeyEvent(ImGuiKey_A, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  frame();
  io.AddInputCharactersUTF8("116");
  frame(); frame();
  io.AddKeyEvent(ImGuiKey_Enter, true);
  frame();
  io.AddKeyEvent(ImGuiKey_Enter, false);
  frame(); frame();
  check(runtime::nr_adaptive_target_fps.load() == 116.f, "visible numeric input commits an exact 116 FPS target");
  float stored = 0.f;
  reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetAdaptiveTargetFps.key, stored);
  check(stored == 116.f, "exact numeric target persists through the production Setting boundary");
  actual.passes[0].execution_confirmed = false;
  frame(); frame();
  check(find("##actual_resolution_scale").disabled && std::ranges::any_of(rendered_text, [](const auto& item) {
    return item.text.find("Pending confirmation") != std::string::npos;
  }), "unknown actual size is pending while the saved manual value remains hidden");
  runtime::nr_adaptive_enabled = false;
  frame(); frame();
  check(!find("##resolution_scale").disabled && runtime::nr_resolution_scale.load() == .75f,
        "disabling adaptive restores the editable original manual upper limit");
  runtime::nr_adaptive_enabled = true;
  runtime::stack_passes = 2;
  runtime::ui_selected_pass = 1;
  runtime::pass_resolution_mode[0] = 2;
  runtime::pass_resolution_scale[0] = .58f;
  adaptive_pass_fixture = true;
  frame(); frame();
  check(find("##actual_resolution_scale").disabled && runtime::pass_resolution_scale[0].load() == .58f,
        "Pass 2 shares the read-only actual editor and preserves its independent upper limit");
  check(std::none_of(items.begin(), items.end(), [](const auto& entry) {
    return entry.second.label == "##pass_resolution_scale" || entry.second.label == "##pass_resolution_presets";
  }), "adaptive Pass 2 hides every manual resolution editing control");
  runtime::nr_adaptive_enabled = false;
  frame(); frame();
  check(!find("##pass_resolution_scale").disabled && runtime::pass_resolution_scale[0].load() == .58f,
        "disabling adaptive restores the independent Pass 2 editor");
  adaptive_pass_fixture = false;
  adaptive_editor_fixture = false;
  resolution_observed_fixture = nullptr;
  summary_fixture = true;
  summary_fields.fps = "153";
  summary_fields.gpu = "6.6ms";
  summary_fields.fg = "4x";
  summary_fields.resolution_ratio = "67%";
  if (std::getenv("NR_NEXUS_UX_REPRO") != nullptr) {
    const float saved_height = panel_height;
    panel_height = 240.f;
    for (const auto& [language, suffix] : {std::pair{0, "en"}, std::pair{13, "zh"}}) {
      ui::SelectLanguage(language);
      runtime::ui_language_choice = language + 1;
      for (const int width : {1000, 340}) {
        panel_width = static_cast<float>(width);
        capture((std::string("adaptive-summary-filled-") + suffix + "-" + std::to_string(width)).c_str());
      }
    }
    panel_height = saved_height;
    runtime::ui_language_choice = 1;
    ui::SelectLanguage(0);
  }
  for (const auto width : {1000.f, 340.f, 220.f}) {
    panel_width = width;
    frame(); frame();
    const auto locate = [&](const char* label) {
      const auto found = std::ranges::find_if(rendered_text, [label](const auto& item) { return item.text == label; });
      check(found != rendered_text.end(), "all four compact metric labels and values remain visible");
      return found->rect;
    };
    const auto fps = locate("FPS");
    const auto gpu = locate("NR GPU");
    const auto fg = locate("FG");
    const auto resolution = locate("NR resolution");
    check(locate("153").Min.y > fps.Min.y && locate("6.6ms").Min.y > gpu.Min.y
              && locate("4x").Min.y > fg.Min.y && locate("67%").Min.y > resolution.Min.y,
          "each compact metric places its value on the second line");
    check(std::abs(locate("153").GetCenter().x - fps.GetCenter().x) < 1.f
              && std::abs(locate("6.6ms").GetCenter().x - gpu.GetCenter().x) < 1.f
              && std::abs(locate("4x").GetCenter().x - fg.GetCenter().x) < 1.f
              && std::abs(locate("67%").GetCenter().x - resolution.GetCenter().x) < 1.f,
          "metric labels and values share each column's center");
    if (width >= 340.f) {
      check(fps.Min.x < gpu.Min.x && gpu.Min.x < fg.Min.x && fg.Min.x < resolution.Min.x
                && std::abs(fps.Min.y - resolution.Min.y) < 1.f,
            "wide and game-sized 340 px status fit four ordered metric columns");
      const auto first = std::ranges::find_if(rendered_text, [](const auto& item) { return item.text == "FPS"; });
      const auto& content = first->window->WorkRect;
      check(std::abs((fps.Min.x - content.Min.x) - (content.Max.x - resolution.Max.x)) < 1.f,
            "four metric columns leave symmetric text padding at the content edges");
    } else {
      check(gpu.Min.y > fps.Min.y && fg.Min.y > gpu.Min.y && resolution.Min.y > fg.Min.y,
            "narrow status stacks complete metric labels without clipping");
    }
  }
  for (int language = 0; language < ui::kLanguageCount; ++language) {
    ui::SelectLanguage(language);
    for (const float width : {1000.f, 340.f, 220.f}) {
      panel_width = width;
      frame(); frame();
      for (const char* key : {"FPS", "NR GPU", "FG", "NR resolution", "67%"}) {
        check(std::ranges::any_of(rendered_text, [&](const auto& item) { return item.text == ui::Tr(key); }),
              "four summary fields retain their complete labels and values in every language and width");
      }
      for (const auto& item : rendered_text) {
        if (std::strstr(item.window->Name, "##nr_status_summary") == nullptr) continue;
        check(item.rect.Max.x <= item.window->InnerClipRect.Max.x + 1.f
                  && item.rect.Max.y <= item.window->InnerClipRect.Max.y + 1.f,
              "wrapped summary fields stay inside the compact card");
      }
    }
  }
  ui::SelectLanguage(0);
  check(ui::SummaryMetricColumns(summary_fields, 1000.f) == 3,
        "the runtime monitor retains its existing three-column metric layout");
  runtime::MonitorCadence cadence;
  ui::frame_metrics::Sample raw{.source = ui::frame_metrics::Source::PRESENT, .raw_interval_ms = 28.f,
      .interval_ms = 1000.f / 35.f, .raw_valid = true, .valid = true};
  namespace rate = renodx::addons::dlss5::output_framerate;
  rate::Snapshot output{.available = true, .source = rate::Source::XEFG_STATUS, .fps = 153.4};
  cadence.FormatRate(output, raw);
  check(cadence.text == "153" && std::string(cadence.fps_tip) == rate::SourceText(output.source)
            && raw.raw_interval_ms == 28.f,
        "shared FPS formatting prefers output without manufacturing a frame interval");
  output.available = false;
  cadence.FormatRate(output, raw);
  check(cadence.text == "35" && std::string(cadence.fps_tip) == ui::frame_metrics::SourceTip(raw.source),
        "unknown output falls back to accurately labelled current cadence");
  const auto summary_for = [&](rate::Snapshot snapshot) {
    {
      std::unique_lock lock(rate::internal::state.mutex);
      auto& published = rate::internal::state.present.at(output_runtime.output.swapchain);
      auto& stream = published.stream;
      stream.snapshot = snapshot;
      stream.identity = output_runtime.output.swapchain;
      stream.previous_ns = runtime::SteadyNowNs();
      if (snapshot.source == rate::Source::NATIVE_STATE) {
        // The synthetic SDK estimate covers this output's current application-frame interval.
        published.native.application_ns = stream.previous_ns;
        ++published.native.coverage;
        published.native.state_coverage = published.native.coverage;
      }
    }
    runtime::monitor_snapshot.attempted_ns = 0;
    return refresh_monitor().summary;
  };
  check(summary_for({}).fg == "—", "unknown FG is not labelled disabled");
  check(summary_for({.fg_off = true}).fg == ui::Tr("Not enabled"), "confirmed disabled FG has a distinct value");
  const auto observed = summary_for({.available = true, .source = rate::Source::NATIVE_STATE, .fps = 153.,
      .observed_multiplier = 3.99});
  check(observed.fg == "4x" && observed.fps == "153", "observed near-integer FG and output FPS share concise values");
  check(observed.fg_tip.find("Observed") != std::string::npos, "FG estimation and source details remain discoverable in its tooltip");
  const auto saved_route = runtime::ui_fg_route;
  const auto saved_hook = runtime::hook_point.load();
  const bool saved_vulkan = runtime::vulkan_present_seen.load();
  const bool saved_unusable = runtime::present_unusable.load();
  runtime::hook_point = runtime::kHookPresent;
  runtime::vulkan_present_seen = false;
  runtime::present_unusable = false;
  runtime::ui_fg_route.route = ui::FgRoute::kBeforeFg;
  runtime::ui_fg_route.previous.multiplier = 1;
  fg_route_fixture = true;
  frame(); frame();
  check(rendered_text.empty(), "normal Pre-FG route adds no duplicate status row beside the shared SDK FG value");
  fg_route_detailed = true;
  frame(); frame();
  check(std::ranges::any_of(rendered_text, [](const auto& item) { return item.text == ui::Tr(ui::text::kFgBefore); })
            && std::ranges::none_of(rendered_text, [](const auto& item) {
              return item.text == ui::Tr(ui::text::kFgBeforeUnknown) || item.text == ui::Tr(ui::text::kFgUnknownMultiplierTip);
            }),
        "detailed Pre-FG diagnostics retain the route without contradicting reliable SDK multiplier evidence");
  runtime::ui_fg_route.route = ui::FgRoute::kFallbackPresent;
  fg_route_detailed = false;
  frame(); frame();
  check(std::ranges::any_of(rendered_text, [](const auto& item) { return item.text == ui::Tr(ui::text::kFgFallbackShort); }),
        "compact Pre-FG fallback warnings remain visible");
  fg_route_fixture = false;
  runtime::ui_fg_route = saved_route;
  runtime::hook_point = saved_hook;
  runtime::vulkan_present_seen = saved_vulkan;
  runtime::present_unusable = saved_unusable;
  rate::Invalidate(output_runtime.output);
  summary_fixture = false;
  panel_width = 1000.f;
  hook_editor_fixture = true;
  runtime::nr_adaptive_enabled = true;
  runtime::nr_follow_input_res = false;
  runtime::nr_resolution_scale = 1.f;
  runtime::preset = 2;
  namespace xefg = renodx::addons::dlss5::fg::xefg;
  const auto saved_requested = xefg::internal::requested.load();
  const bool saved_owns = xefg::internal::owns_stream.load();
  xefg::internal::requested = runtime::fg_service::Requested;
  xefg::internal::owns_stream = true;
  runtime::hooks_enabled = true;
  runtime::enabled = true;
  for (const uint32_t passes : {1u, 2u}) {
    runtime::stack_passes = passes;
    runtime::pass_resolution_mode[0] = 2;
    runtime::pass_resolution_scale[0] = 1.f;
    runtime::pass_stage[0] = 1;
    for (const auto& [label, stored] : {std::pair{"Upscaled", 0u}, {"Render", 1u}}) {
      runtime::hook_point = runtime::kHookPresent;
      frame(); frame();
      check(!find(label).disabled, "adaptive Present permits a supported return to inline execution");
      click(find(label));
      if (runtime::stage_edit::pending) click(find("confirm_stage_change"));
      check(runtime::hook_point.load() == stored, "adaptive Hook click applies the requested inline stage");
    }
  }
  hook_editor_fixture = false;
  runtime::stack_passes = 1;
  runtime::ui_menu_page = 0;
  runtime::ui_classic_layout = false;
  for (const bool minimum_reached : {false, true}) {
    for (const auto& [label, stored] : {std::pair{"Upscaled", 0u}, {"Render", 1u}}) {
      runtime::hook_point = runtime::kHookPresent;
      namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
      adaptive::Session session;
      adaptive::Sizes upper{}, floor{};
      upper[0] = {.width = 3840, .height = 2160};
      floor[0] = {.width = 1267, .height = 713};
      const adaptive::Settings settings{.enabled = true, .target_fps = 116.f, .minimum_scale = .33f, .prepared_levels = 4};
      const auto now = runtime::SteadyNowNs();
      const adaptive::Output output{.available = true, .source = 1, .generation = 1, .configuration = 1,
          .serial = 1, .begin_ns = now - 500'000'000, .end_ns = now, .fps = (minimum_reached ? 50. : 120.)};
      session.Prepare(settings, upper, floor, 42, output, now - 10'000'000'000, true, false);
      session.Request(minimum_reached ? session.ladder.count - 1 : 0, now - 10'000'000'000);
      // Complete owner readiness and bind the output before collecting stable samples.
      const auto prepared_levels = static_cast<uint32_t>(session.ladder.count);
      session.UpdatePreparation(session.preparation_epoch, prepared_levels, prepared_levels,
          adaptive::Reason::PREPARING, now - 10'000'000'000);
      session.Applied(session.status.generation, session.status.requested, now - 10'000'000'000);
      session.Prepare(settings, upper, floor, 42, output, now - 10'000'000'000, true, false);
      for (uint64_t sample = 0; sample < 5; ++sample) {
        session.ObserveGpu({.generation = session.status.generation, .frame = sample + 1, .sequence = sample + 1,
            .recorded_ns = now - 1'500'000'000 + static_cast<int64_t>(sample) * 250'000'000,
            .completed_ns = now - 500'000'000, .total_us = 1000., .valid = true, .stable = true,
            .sampling_generation = session.sampling_generation}, now);
      }
      session.Prepare(settings, upper, floor, 42, output, now, true, false);
      check(session.status.reason == (minimum_reached ? adaptive::Reason::AT_MINIMUM : adaptive::Reason::OBSERVING),
          "the held-click fixture starts from normal or minimum-reached controller status");
      publish_display_status(session.status, DisplayStage::PRESENT, display_source_generation);
      frame(); frame();
      check(find("##actual_resolution_scale").disabled && !find(label).disabled,
            "the complete Quality page restores interaction after the adaptive read-only control");
      auto button = find(label);
      for (int attempt = 0; attempt < 4 && !button.window->InnerClipRect.Contains(button.rect.GetCenter()); ++attempt) {
        ImGui::SetScrollY(button.window, button.window->Scroll.y + button.rect.GetCenter().y
            - button.window->InnerClipRect.GetCenter().y);
        frame(); frame();
        button = find(label);
      }
      const auto pressed_at = button.rect.GetCenter();
      io.AddMousePosEvent(pressed_at.x, pressed_at.y);
      frame();
      io.AddMouseButtonEvent(0, true);
      frame();
      check(!runtime::nr_adaptive_editing.load(), "holding a Stage button never masquerades as an unpublished slider edit");
      session.Prepare(settings, upper, floor, 42, output, now, true, runtime::nr_adaptive_editing.load());
      publish_display_status(session.status, DisplayStage::PRESENT, display_source_generation);
      frame();
      check(std::abs(find(label).rect.Min.y - button.rect.Min.y) < .5f,
          "temporary adaptive pause cannot move the pressed Stage button before mouse release");
      io.AddMouseButtonEvent(0, false);
      frame(); frame();
      if (runtime::stage_edit::pending) click(find("confirm_stage_change"));
      check(runtime::hook_point.load() == stored,
            "the complete Quality page applies adaptive Present to inline Hook changes");
    }
  }
  runtime::ui_selected_pass = 0;
  runtime::kSetResolutionScale.Commit(.75f);
  runtime::kSetAdaptiveEnabled.Commit(0.f);
  runtime::kSetHookPoint.Commit(static_cast<float>(runtime::kHookPresent));
  const auto toggle_adaptive = [&](bool enabled) {
    adaptive_editor_fixture = true;
    resolution_observed_fixture = &actual;
    actual.passes[0].execution_confirmed = true;
    frame(); frame();
    const auto editor = find(runtime::nr_adaptive_enabled.load() ? "##actual_resolution_scale" : "##resolution_scale");
    const auto setting_id = ImHashStr(runtime::kSetAdaptiveEnabled.key, 0, editor.window->IDStack[0]);
    const auto switch_id = ImHashStr("##value", 0, setting_id);
    check(items.contains(switch_id) && !items.at(switch_id).disabled,
        "the production adaptive switch is interactive throughout the continuous transition");
    click(items.at(switch_id));
    bool stored_enabled = !enabled;
    float stored_upper = 0.f;
    reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetAdaptiveEnabled.key, stored_enabled);
    reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetResolutionScale.key, stored_upper);
    check(runtime::nr_adaptive_enabled.load() == enabled && stored_enabled == enabled
        && runtime::nr_resolution_scale.load() == .75f && stored_upper == .75f,
        "adaptive switch state and saved preference agree while retaining the manual upper limit");
    check(find(enabled ? "##actual_resolution_scale" : "##resolution_scale").disabled == enabled,
        "On shows the actual read-only scale and Off restores the saved editable scale");
    adaptive_editor_fixture = false;
    resolution_observed_fixture = nullptr;
    frame(); frame();
  };
  toggle_adaptive(true);
  runtime::active_features = 1;
  for (auto* samples : {renodx::addons::dlss5::gpu_timers::stage_stats,
           renodx::addons::dlss5::gpu_timers::execution_stats, renodx::addons::dlss5::gpu_timers::completed_stats}) {
    for (size_t stage = 0; stage < 3; ++stage) samples[stage] = {};
  }
  const auto confirm_stage_size = [&] {
    namespace timers = renodx::addons::dlss5::gpu_timers;
    runtime::monitor_snapshot.attempted_ns = 0;
    auto snapshot = refresh_monitor();
    check(snapshot.summary.resolution_ratio.empty() && !snapshot.monitor.passes[0].execution_confirmed,
        "a Stage transition never reuses the previous Stage's completed dimensions");
    adaptive_editor_fixture = true;
    resolution_observed_fixture = &snapshot.monitor;
    frame(); frame();
    check(find("##actual_resolution_scale").disabled && std::ranges::any_of(rendered_text, [](const auto& item) {
      return item.text.find("Pending confirmation") != std::string::npos;
    }), "the actual scale remains read-only and pending until the new Stage completes");
    const bool render = runtime::hook_point.load() == runtime::kHookRender;
    const size_t stage = (render ? 0 : runtime::hook_point.load() == runtime::kHookUpscaled ? 1 : 2);
    auto& sample = timers::stage_stats[stage];
    sample = {};
    sample.valid = true;
    sample.state = timers::SampleState::VALID;
    sample.sequence = ++timers::internal::next_sequence;
    sample.recorded_ns = sample.measured_ns = runtime::SteadyNowNs();
    sample.width = (render ? 1920 : 3840);
    sample.height = (render ? 1080 : 2160);
    sample.passes[0] = {.id = 0, .model = runtime::preset.load(), .width = (render ? 960u : 2560u),
        .height = (render ? 540u : 1440u), .output_width = (render ? 960u : 2560u),
        .output_height = (render ? 540u : 1440u), .state = timers::SampleState::VALID};
    std::strcpy(sample.path, (render ? "preSR" : stage == 1 ? "after" : "present"));
    runtime::comparison_ui::CaptureSettings(&sample.identity);
    sample.identity.device_session = timers::device_session;
    sample.identity.stable_epoch = runtime::comparison_stable_epochs[stage];
    sample.identity.source_generation = display_source_generation;
    sample.identity.display_output_generation = output_runtime.output.generation;
    runtime::stage_telemetry[stage].last_evaluate = {.source_generation = display_source_generation};
    publish_completion(sample);
    publish_display_status({.reason = adaptive::Reason::OBSERVING}, static_cast<DisplayStage>(stage), display_source_generation);
    if (stage == static_cast<size_t>(DisplayStage::UPSCALED)) {
      const auto saved_api = runtime::stage_source_api.exchange(renodx::addons::dlss5::capability::Api::VULKAN);
      const auto status = runtime::CopyAdaptiveStatus();
      runtime::stage_source_api = saved_api;
      check(status.reason == adaptive::Reason::OBSERVING
                && status.display_output_generation == output_runtime.output.generation
                && status.display_source_generation == display_source_generation,
            "a Vulkan source observation preserves the matched D3D12 output's adaptive status");
    }
    runtime::monitor_snapshot.attempted_ns = 0;
    snapshot = refresh_monitor();
    const auto& current = snapshot.monitor.passes[0];
    const char* actual_size = (render ? "960 x 540 (50%)" : "2560 x 1440 (67%)");
    const char* reference = (render ? "1920 x 1080" : "3840 x 2160");
    check(current.execution_confirmed && current.actual_size == actual_size
        && current.actual_scale == static_cast<float>(sample.passes[0].width) / sample.width
        && current.saved_manual_limit == "75%" && snapshot.summary.resolution_ratio == (render ? "50%" : "67%")
        && snapshot.summary.resolution_tip.find(reference) != std::string::npos,
        "current completion dimensions, Stage reference and displayed ratio agree through the round trip");
    frame(); frame();
    check(find("##actual_resolution_scale").disabled && std::ranges::any_of(rendered_text, [&](const auto& item) {
      return item.text.find(actual_size) != std::string::npos;
    }), "the read-only editor renders the current Stage's confirmed working dimensions");
    adaptive_editor_fixture = false;
    resolution_observed_fixture = nullptr;
    frame(); frame();
  };
  confirm_stage_size();
  for (const auto& [label, stage] : {std::pair{"Upscaled", runtime::kHookUpscaled},
           {"Render", runtime::kHookRender}, {"Upscaled", runtime::kHookUpscaled}, {"Present", runtime::kHookPresent}}) {
    check(!find(label).disabled, "continuous adaptive Stage changes remain interactive on the complete Quality page");
    click(find(label));
    if (runtime::stage_edit::pending) click(find("confirm_stage_change"));
    uint32_t stored_stage = UINT32_MAX;
    bool stored_enabled = false;
    float stored_upper = 0.f;
    reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetHookPoint.key, stored_stage);
    reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetAdaptiveEnabled.key, stored_enabled);
    reshade::get_config_value(nullptr, runtime::kConfigSection, runtime::kSetResolutionScale.key, stored_upper);
    check(runtime::hook_point.load() == stage && stored_stage == stage && runtime::nr_adaptive_enabled.load()
        && stored_enabled && stored_upper == .75f && runtime::nr_resolution_scale.load() == .75f
        && !runtime::nr_adaptive_editing.load(),
        "Present to Upscaled to Render and back preserves the saved adaptive state and manual upper limit");
    confirm_stage_size();
  }
  toggle_adaptive(false);
  toggle_adaptive(true);
  std::puts("ADAPTIVE_UI continuous Off-On-Present-Upscaled-Render-Upscaled-Present-Off-On passed");
  auto minimum_slider = find("##adaptive_minimum");
  for (int attempt = 0; attempt < 4 && !minimum_slider.window->InnerClipRect.Contains(minimum_slider.rect.GetCenter()); ++attempt) {
    ImGui::SetScrollY(minimum_slider.window, minimum_slider.window->Scroll.y + minimum_slider.rect.GetCenter().y
        - minimum_slider.window->InnerClipRect.GetCenter().y);
    frame(); frame();
    minimum_slider = find("##adaptive_minimum");
  }
  const auto minimum_position = minimum_slider.rect.GetCenter();
  io.AddMousePosEvent(minimum_position.x, minimum_position.y);
  frame();
  io.AddMouseButtonEvent(0, true);
  frame();
  io.AddMousePosEvent(minimum_position.x + 10.f, minimum_position.y);
  frame();
  check(runtime::slider_pending == &runtime::kSetAdaptiveMinScale && runtime::nr_adaptive_editing.load(),
      "an actual minimum-scale drag retains the production adaptive pause");
  std::shared_ptr<renodx::addons::dlss5::adaptive_resolution::Session> editing_session;
  const auto editing_plan = runtime::RequestedStagePlan();
  runtime::FreezeAdaptiveFrame(&editing_session, editing_plan, editing_plan.At(editing_plan.effective[0]),
      3840, 2160, 1920, 1080, 0xADA57A6E, true);
  check(editing_session->status.reason == renodx::addons::dlss5::adaptive_resolution::Reason::FROZEN,
      "real slider edits still freeze evaluation while Stage-button holds do not");
  io.AddMouseButtonEvent(0, false);
  frame(); frame();
  check(runtime::slider_pending == nullptr && !runtime::nr_adaptive_editing.load(),
      "releasing the actual slider settles the edit and resumes adaptive evaluation");
  xefg::internal::requested = saved_requested;
  xefg::internal::owns_stream = saved_owns;
  hook_editor_fixture = false;
  {
    namespace addon = renodx::addons::dlss5;
    namespace timers = addon::gpu_timers;
    runtime::hook_point = runtime::kHookUpscaled;
    runtime::stack_passes = 1;
    runtime::active_features = 1;
    runtime::nr_adaptive_enabled = false;
    runtime::card_observation.window = {};
    addon::NrVerdict verdict{.state = addon::NrState::kEngaged, .api = "d3d12", .path = "inline"};
    verdict.eligible = verdict.injected = 100;
    const auto started = runtime::SteadyNowNs();
    auto& measured = timers::stage_stats[1];
    measured = {};
    timers::execution_stats[1] = {};
    timers::internal::next_sequence = 10;
    runtime::CollectCardInputs(verdict, started);
    measured.valid = true;
    measured.state = timers::SampleState::VALID;
    measured.sequence = ++timers::internal::next_sequence;
    measured.recorded_ns = measured.measured_ns = started + 100'000'000LL;
    runtime::comparison_ui::CaptureSettings(&measured.identity);
    measured.identity.device_session = timers::device_session;
    measured.passes[0] = {.id = 0, .state = timers::SampleState::VALID};
    verdict.eligible = verdict.injected = 110;
    check(runtime::CollectCardInputs(verdict, started + 100'000'000LL).recent->gpu_completed,
          "a current GPU completion establishes card execution evidence");
    const auto completed = measured;
    measured.state = timers::SampleState::REPLAYED;
    measured.sequence = ++timers::internal::next_sequence;
    verdict.eligible = verdict.injected = 120;
    check(runtime::CollectCardInputs(verdict, started + 200'000'000LL).recent->gpu_completed,
          "a rejected newer recording cannot erase a still-current completed execution");
    auto& observed = runtime::stage_telemetry[1].last_evaluate;
    observed.observed_ns = started + 250'000'000LL;
    observed.api_returned = true;
    observed.result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
    check(!runtime::CollectCardInputs(verdict, started + 250'000'000LL).recent->gpu_completed,
          "a real Evaluate failure immediately invalidates retained execution evidence");
    observed = {};
    measured = completed;
    runtime::CollectCardInputs(verdict, started + 300'000'000LL);
    check(!runtime::CollectCardInputs(verdict, started + ui::RecentHealthWindow::kStaleNs + 100'000'001LL)
               .recent->gpu_completed,
          "retained execution evidence expires at the existing two-second bound");
    measured = completed;
    measured.sequence = ++timers::internal::next_sequence;
    measured.recorded_ns = measured.measured_ns = started + 300'000'000LL;
    ++runtime::configuration_generation;
    check(!runtime::CollectCardInputs(verdict, started + 300'000'000LL).recent->gpu_completed,
          "a settings change rejects both latest and retained old-configuration completions");
    runtime::nr_adaptive_enabled = true;
    runtime::nr_resolution_scale = .42f;
    for (const auto hook : {runtime::kHookRender, runtime::kHookUpscaled, runtime::kHookPresent}) {
      runtime::hook_point = hook;
      const size_t stage = (hook == runtime::kHookRender ? 0 : hook == runtime::kHookUpscaled ? 1 : 2);
      auto& sample = timers::stage_stats[stage];
      sample = completed;
      sample.sequence = ++timers::internal::next_sequence;
      sample.recorded_ns = sample.measured_ns = runtime::SteadyNowNs();
      sample.width = (hook == runtime::kHookRender ? 2560 : 3840);
      sample.height = (hook == runtime::kHookRender ? 1440 : 2160);
      sample.passes[0].width = (hook == runtime::kHookRender ? 1715 : 2573);
      sample.passes[0].height = (hook == runtime::kHookRender ? 965 : 1447);
      std::strcpy(sample.path, (stage == 0 ? "preSR" : stage == 1 ? "after" : "present"));
      runtime::comparison_ui::CaptureSettings(&sample.identity);
      sample.identity.device_session = timers::device_session;
      sample.identity.stable_epoch = runtime::comparison_stable_epochs[stage];
      sample.identity.source_generation = display_source_generation;
      sample.identity.display_output_generation = output_runtime.output.generation;
      sample.passes[0].model = runtime::preset.load();
      sample.passes[0].output_width = sample.passes[0].width;
      sample.passes[0].output_height = sample.passes[0].height;
      runtime::stage_telemetry[stage].last_evaluate = {.source_generation = display_source_generation};
      publish_completion(sample);
      publish_display_status({.reason = adaptive::Reason::OBSERVING}, static_cast<DisplayStage>(stage), display_source_generation);
      runtime::monitor_snapshot.attempted_ns = 0;
      const auto actual_snapshot = refresh_monitor();
      const auto reference = std::to_string(sample.width) + " x " + std::to_string(sample.height);
      const auto actual_size = std::to_string(sample.passes[0].width) + " x " + std::to_string(sample.passes[0].height);
      check(actual_snapshot.summary.resolution_ratio == "67%"
                && actual_snapshot.monitor.passes[0].actual_scale == static_cast<float>(sample.passes[0].width) / sample.width
                && actual_snapshot.summary.resolution_tip.find(reference) != std::string::npos
                && actual_snapshot.summary.resolution_tip.find(actual_size) != std::string::npos,
            "the fourth field shares Pass 1 completed working dimensions and the selected Stage reference");
      sample.state = timers::SampleState::EXPIRED;
      const auto expired = runtime::SteadyNowNs() - timers::internal::kSampleAgeNs - 1;
      runtime::card_observation.completions[stage].recorded_ns = expired;
      runtime::adaptive_completion_scopes[stage].at({output_runtime.output.generation, display_source_generation})
          .retained.recorded_ns = expired;
      runtime::monitor_snapshot.attempted_ns = 0;
      check(refresh_monitor().summary.resolution_ratio.empty(),
            "an unknown actual ratio never falls back to the saved manual percentage");
    }
    const auto saved_policy = xefg::internal::requested.load();
    const bool saved_stream_owned = xefg::internal::owns_stream.load();
    const auto saved_declaration = runtime::native_fg_declaration.exchange(runtime::NativeFgDeclaration::kOff);
    constexpr uintptr_t secondary_swapchain = 0xADA600;
    const std::array<uintptr_t, 1> secondary_buffers{0xADA700};
    rate::RegisterPresentOutput(secondary_swapchain, output_runtime.device.get_native(), 0, secondary_buffers);
    const auto secondary_output = rate::FindPresentOutput(output_runtime.device.get_native(), secondary_buffers[0]);
    xefg::internal::requested = runtime::fg_service::Requested;
    for (const bool private_fg : {false, true}) {
      runtime::hook_point = (private_fg ? runtime::kHookPresent : runtime::kHookUpscaled);
      xefg::internal::owns_stream = private_fg;
      const size_t stage = (private_fg ? 2 : 1);
      auto& sample = timers::stage_stats[stage];
      auto& proof = timers::execution_stats[stage];
      const runtime::AdaptiveCompletionKey primary_key{output_runtime.output.generation, 99};
      const auto evaluation_stage = static_cast<DisplayStage>(stage);
      const auto observe = [&](const runtime::AdaptiveCompletionKey& key, bool reset, bool api_returned) {
        const auto& [output, source] = key;
        auto session = std::make_shared<adaptive::Session>();
        session->status.display_output_generation = output;
        session->status.display_source_generation = source;
        session->status.display_stage = static_cast<uint32_t>(stage);
        const adaptive::Frame frame{.session = session};
        addon::RuntimeLock lock(runtime::runtime_mutex);
        const runtime::AdaptiveFrameScope scope(&frame);
        if (reset) {
          runtime::ObserveLocalHistoryReset(evaluation_stage, source, runtime::kResetDimensions);
        } else {
          runtime::FeatureState feature;
          feature.source_generation = source;
          runtime::ObserveNrEvaluation(evaluation_stage, nullptr, feature, {}, 0,
              NVSDK_NGX_Result_FAIL_UnableToInitializeFeature, runtime::RequestedStagePlan(), nullptr, api_returned);
        }
      };
      const auto seed = [&] {
        runtime::stage_telemetry[stage].last_evaluate = {.source_generation = 99};
        proof = {};
        sample = completed;
        sample.sequence = ++timers::internal::next_sequence;
        sample.recorded_ns = sample.measured_ns = runtime::SteadyNowNs();
        sample.width = 3840;
        sample.height = 2160;
        sample.total_us = 6600;
        sample.passes[0] = {.id = 0, .model = runtime::preset.load(), .width = 2573, .height = 1447,
            .output_width = 2573, .output_height = 1447, .total_us = 6600, .state = timers::SampleState::VALID};
        std::strcpy(sample.path, (private_fg ? "present" : "after"));
        runtime::comparison_ui::CaptureSettings(&sample.identity);
        sample.identity.source_generation = 99;
        sample.identity.display_output_generation = output_runtime.output.generation;
        sample.identity.stable_epoch = runtime::comparison_stable_epochs[stage];
        sample.identity.device_session = timers::device_session;
        publish_completion(sample);
        publish_display_status({.reason = adaptive::Reason::OBSERVING}, static_cast<DisplayStage>(stage), 99);
        runtime::monitor_snapshot.attempted_ns = 0;
        const auto snapshot = refresh_monitor();
        check(snapshot.summary.resolution_ratio == "67%" && snapshot.summary.gpu == "6.6ms",
              "inline and private FG share the same current completed size and measured GPU source");
        sample.state = timers::SampleState::REPLAYED;
        sample.sequence = ++timers::internal::next_sequence;
      };
      seed();
      runtime::monitor_snapshot.attempted_ns = 0;
      const auto rejected = refresh_monitor();
      check(rejected.summary.resolution_ratio == "67%" && rejected.summary.gpu.empty()
                && rejected.monitor.passes[0].execution_confirmed,
            "a rejected latest recording retains bounded actual size while leaving GPU cost unavailable");
      proof = runtime::card_observation.completions[stage];
      proof.sequence = ++timers::internal::next_sequence;
      proof.recorded_ns = proof.measured_ns = runtime::SteadyNowNs();
      proof.passes[0].width = 1920;
      proof.passes[0].height = 1080;
      publish_completion(proof);
      runtime::monitor_snapshot.attempted_ns = 0;
      check(refresh_monitor().summary.resolution_ratio == "50%",
            "a new completed working size takes priority over retained actual dimensions");
      // Model the process-wide declaration published by OptionsOn on the secondary output.
      runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOn;
      const auto after_fg_declaration = refresh_monitor();
      check(after_fg_declaration.summary.resolution_ratio == "50%"
                && after_fg_declaration.monitor.passes[0].execution_confirmed,
            "a secondary native FG On declaration preserves the selected output's completed size");
      runtime::native_fg_declaration = runtime::NativeFgDeclaration::kOff;
      const float saved_blend = runtime::kSetControlMaskBlendMode.Get();
      runtime::kSetControlMaskBlendMode.Store(saved_blend == 0.f ? 1.f : 0.f);
      const auto after_blend = refresh_monitor();
      check(after_blend.summary.resolution_ratio == "50%" && after_blend.monitor.passes[0].execution_confirmed,
            "a Blend edit preserves the selected output's completed size");
      runtime::kSetControlMaskBlendMode.Store(saved_blend);
      runtime::PublishAdaptiveStatus({.reason = adaptive::Reason::FAILED,
          .display_output_generation = secondary_output.generation, .display_source_generation = 100,
          .display_stage = static_cast<uint32_t>(stage)});
      runtime::monitor_snapshot.attempted_ns = 0;
      check(refresh_monitor().summary.resolution_ratio == "50%",
          "a later secondary-output publication preserves the selected runtime's completed dimensions");
      proof = {};
      observe({secondary_output.generation, 99}, true, true);
      runtime::monitor_snapshot.attempted_ns = 0;
      check(refresh_monitor().summary.resolution_ratio == "50%",
          "same Stage and source on a different output can reset while the selected output retains its completed size");
      observe({output_runtime.output.generation, 100}, false, true);
      runtime::monitor_snapshot.attempted_ns = 0;
      const auto secondary_failure = refresh_monitor();
      check(secondary_failure.summary.resolution_ratio == "50%" && secondary_failure.monitor.passes[0].execution_confirmed,
          "same Stage and output with a different source can fail while the selected source retains its completed size");
      for (int rejection = 0; rejection < 11; ++rejection) {
        seed();
        auto& scope = runtime::adaptive_completion_scopes[stage].at(primary_key);
        switch (rejection) {
          case 0: {
            const auto expired = runtime::SteadyNowNs() - timers::internal::kSampleAgeNs - 1;
            runtime::card_observation.completions[stage].recorded_ns = expired;
            scope.retained.recorded_ns = expired;
            break;
          }
          case 1: ++runtime::configuration_generation; break;
          case 2: ++timers::device_session; break;
          case 3: {
            const auto* released = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xADA800});
            addon::RuntimeLock lock(runtime::runtime_mutex);
            runtime::features[released].source_generation = 99;
            scope.has_source = true;
            runtime::ForgetSourceHandleLocked(released);
            break;
          }
          case 4:
            ++runtime::card_observation.completions[stage].identity.source_generation;
            ++scope.retained.identity.source_generation;
            break;
          case 5: observe(primary_key, true, true); break;
          case 6:
          case 7:
            observe(primary_key, false, rejection == 7);
            break;
          case 8:
            runtime::card_observation.completions[stage].identity.display_output_generation = secondary_output.generation;
            scope.retained.identity.display_output_generation = secondary_output.generation;
            break;
          case 9:
            publish_display_status({.reason = adaptive::Reason::OBSERVING}, static_cast<DisplayStage>(stage), 100);
            break;
          case 10:
            std::strcpy(runtime::card_observation.completions[stage].path, (stage == 2 ? "after" : "present"));
            std::strcpy(scope.retained.path, (stage == 2 ? "after" : "present"));
            break;
        }
        runtime::monitor_snapshot.attempted_ns = 0;
        const auto invalid = refresh_monitor();
        check(invalid.summary.resolution_ratio.empty() && invalid.summary.gpu.empty()
                  && !invalid.monitor.passes[0].execution_confirmed,
              "expired, changed, released or truly failed execution cannot retain actual dimensions");
      }
      constexpr uint64_t first_read_source = 101;
      auto first = sample;
      first.state = timers::SampleState::VALID;
      first.sequence = ++timers::internal::next_sequence;
      first.recorded_ns = first.measured_ns = runtime::SteadyNowNs();
      first.identity.source_generation = first_read_source;
      publish_display_status({.reason = adaptive::Reason::OBSERVING}, evaluation_stage, first_read_source);
      publish_completion(first);
      auto later = first;
      later.sequence = ++timers::internal::next_sequence;
      later.recorded_ns = later.measured_ns = runtime::SteadyNowNs();
      later.identity.display_output_generation = secondary_output.generation;
      later.identity.source_generation = 100;
      later.passes[0].width = 1920;
      later.passes[0].height = 1080;
      publish_completion(later);
      sample = proof = later;
      const auto first_read = refresh_monitor();
      check(timers::completed_stats[stage].sequence == later.sequence
                && first_read.summary.resolution_ratio == "67%" && first_read.monitor.passes[0].execution_confirmed,
            "the first monitor read finds A's producer-retained dimensions after B replaces every current Stage sample");
    }
    xefg::internal::requested = saved_policy;
    xefg::internal::owns_stream = saved_stream_owned;
    runtime::native_fg_declaration = saved_declaration;
    rate::DestroyPresentOutput(secondary_swapchain);
    runtime::nr_adaptive_enabled = false;
  }
  {
    namespace addon = renodx::addons::dlss5;
    namespace timers = addon::gpu_timers;
    runtime::enabled = true;
    runtime::hooks_enabled = true;
    runtime::hook_point = runtime::kHookUpscaled;
    runtime::stack_passes = 1;
    runtime::stage_source_rr = false;
    runtime::native_fg_owns = false;
    xefg::internal::owns_stream = false;
    runtime::d3d12_present_seen = true;
    runtime::d3d11_present_seen = false;
    runtime::vulkan_present_seen = false;
    runtime::ngx_ever_hooked = true;
    runtime::ngx_dlss_evaluate_seen = true;
    runtime::cmd_hooks_ever_installed = true;
    runtime::gate_ever_opened = true;
    runtime::active_features = 1;
    runtime::direct_load_state = runtime::DirectLoadState::Ready;
    runtime::foreign_nr_seen = false;
    runtime::health_active_mask = 0;
    runtime::card_partial_latched = false;
    runtime::nr_runtime_fault = 0;
    runtime::own_evaluations = 0;
    runtime::intercepted_evaluations = 100;
    runtime::successful_evaluations = 100;
    runtime::successful_pre_sr_evaluations = 0;
    addon::mixed_stage_group_successes = 0;
    addon::mixed_stage_source_successes = 0;
    runtime::present_frames_seen = 0;
    runtime::present_prefg_attempts = 0;
    for (auto& decline : addon::nr_decline_counts) decline = 0;
    runtime::card_observation.window = {};
    runtime::card_observation.completions = {};
    runtime::stage_telemetry[1].last_evaluate = {};
    for (auto* samples : {timers::stage_stats, timers::execution_stats, timers::completed_stats}) {
      for (size_t stage = 0; stage < 3; ++stage) samples[stage] = {};
    }
    timers::internal::next_sequence = 100;
    runtime::CollectCardInputs(ComputeNrVerdict(runtime::CollectFunnelInputs()), runtime::SteadyNowNs());
    auto& proof = timers::execution_stats[1];
    proof = {.width = 3840, .height = 2160, .valid = true, .state = timers::SampleState::VALID,
        .sequence = ++timers::internal::next_sequence, .measured_ns = runtime::SteadyNowNs()};
    proof.recorded_ns = proof.measured_ns;
    std::strcpy(proof.path, "upscaled");
    proof.passes[0] = {.id = 0, .model = runtime::preset.load(), .width = 2560, .height = 1440,
        .output_width = 3840, .output_height = 2160, .state = timers::SampleState::VALID};
    runtime::comparison_ui::CaptureSettings(&proof.identity);
    proof.identity.device_session = timers::device_session;
    proof.identity.stable_epoch = runtime::comparison_stable_epochs[1];
    proof.identity.display_output_generation = runtime::MatchRuntimeOutput(&output_runtime).generation;
    check(proof.identity.display_output_generation != 0,
        "the interleaving proof is bound to the selected registered output");
    check(ui::ComputeStatusCard(runtime::CollectCardInputs(
        ComputeNrVerdict(runtime::CollectFunnelInputs()), runtime::SteadyNowNs())).id == ui::CardId::kActive,
        "the interleaving fixture starts Active with one current completed GPU proof");
    const auto sequence = proof.sequence;
    static std::function<void()> interleave_card_snapshot;
    bool interleaved = false;
    interleave_card_snapshot = [&] {
      runtime::monitor_snapshot_before_lock = nullptr;
      check(!addon::runtime_lock::State::HeldByThisThread(),
          "the actual monitor captured its outer timestamp before taking the runtime lock");
      addon::RuntimeLock lock(runtime::runtime_mutex);
      runtime::intercepted_evaluations = 200;
      runtime::successful_evaluations = 200;
      proof.recorded_ns = proof.measured_ns = runtime::SteadyNowNs();
      check(proof.recorded_ns > runtime::monitor_snapshot.attempted_ns,
          "the selected completion is newer than the monitor's pre-lock sample");
      const auto newer = runtime::CollectCardInputs(ComputeNrVerdict(runtime::CollectFunnelInputs()), runtime::SteadyNowNs());
      check(ui::ComputeStatusCard(newer).id == ui::CardId::kActive,
          "a lifecycle-style observation advances the same health window before the monitor locks");
      interleaved = true;
    };
    runtime::monitor_snapshot_before_lock = [] { interleave_card_snapshot(); };
    runtime::monitor_snapshot = {};
    const auto snapshot = refresh_monitor();
    check(interleaved && proof.sequence == sequence && snapshot.sampled_ns >= proof.recorded_ns
        && snapshot.output.generation == proof.identity.display_output_generation
        && snapshot.inputs.input_width == proof.passes[0].width && snapshot.card.id == ui::CardId::kActive,
        "a monitor arriving after the newer selected completion cannot clear Active using its older outer timestamp");
    check(!snapshot.inputs.recent && snapshot.inputs.verdict.eligible == 0 && snapshot.inputs.verdict.injected == 0,
        "the selected card must not inherit unrelated process-wide proof or counters");
    const auto live_session = runtime::CollectCardInputs();
    check(live_session.recent && live_session.recent->gpu_completed && live_session.verdict.eligible == 200
        && live_session.verdict.injected == 200 && ui::ComputeStatusCard(live_session).id == ui::CardId::kActive,
        "the global session still observes current counters and keeps the same current GPU proof");
    interleave_card_snapshot = {};
    const auto before_mirror = runtime::CollectFunnelInputs();
    {
      const runtime::EvaluateInFlightScope pending_source(true);
      ++runtime::intercepted_evaluations;
      const auto pending = runtime::CollectFunnelInputs();
      check(pending.eligible == before_mirror.eligible + 1 && pending.injected == before_mirror.injected
          && pending.unaccounted == before_mirror.unaccounted,
          "an unclassified wrapper raises raw eligible while the funnel still accounts for its in-flight work");
      check(ui::ComputeStatusCard(runtime::CollectCardInputs()).id == ui::CardId::kActive,
          "the current GPU proof stays Active while that raw wrapper is awaiting classification");
      addon::CountNrDecline(addon::NrDeclineReason::kNestedMirror);
    }
    const auto classified = runtime::CollectFunnelInputs();
    check(classified.eligible == before_mirror.eligible && classified.injected == before_mirror.injected
        && classified.unaccounted == before_mirror.unaccounted,
        "real mirror classification restores eligible without changing success or diagnostic accounting");
    const auto after_mirror = runtime::CollectCardInputs();
    check(proof.sequence == sequence && after_mirror.recent.has_value() && after_mirror.recent->gpu_completed
        && ui::ComputeStatusCard(after_mirror).id == ui::CardId::kActive,
        "eligible-only classification rollback cannot discard the same current GPU proof or return to Checking");
  }
  runtime::edit_reuse_enabled = saved_edit_reuse;
  fixture_timers::completed_observer = saved_completion_observer;
}
#endif
