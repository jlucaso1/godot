/**************************************************************************/
/*  gd_mono.cpp                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "gd_mono.h"

#include "../glue/runtime_interop.h"
#include "../godotsharp_dirs.h"
#include "../thirdparty/coreclr_delegates.h"
#include "../thirdparty/hostfxr.h"
#include "../utils/path_utils.h"
#include "gd_mono_cache.h"

#ifdef DEBUG_ENABLED
#include "core/object/class_db.h"
#endif

#ifdef TOOLS_ENABLED
#include "../editor/hostfxr_resolver.h"
#include "../editor/semver.h"
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/os/thread.h"
#include "core/templates/local_vector.h"

#ifdef UNIX_ENABLED
#include <dlfcn.h>
#endif

#ifndef TOOLS_ENABLED
#ifdef ANDROID_ENABLED
#include "../thirdparty/mono_delegates.h"
#endif
#ifdef WEB_ENABLED
// On Web the Mono runtime is statically linked into the engine module rather than
// resolved at runtime, so the real headers are used instead of function-pointer typedefs.
#include <mono/jit/jit.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/class.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/image.h>
#include <mono/metadata/object.h>
#include <mono/metadata/threads.h>
#include <mono/utils/mono-error.h>

extern "C" {
MonoDomain *mono_wasm_load_runtime_common(int p_debug_level, void *p_log_callback, const char *p_interp_opts);
MonoAssembly *mono_domain_assembly_open(MonoDomain *p_domain, const char *p_name);
int monovm_initialize(int p_property_count, const char **p_keys, const char **p_values);
void *mono_method_get_unmanaged_callers_only_ftnptr(MonoMethod *p_method, MonoError *r_error);
// From wasm-bundled-timezones.a. The pack's own driver.c calls this too; the engine is
// the host here, so it makes the call instead.
void mono_register_timezones_bundle(void);
// The statically linked ICU. The shim redirects System.Globalization.Native's dynamic
// lookups at the linked libraries; the data has to be handed over separately because it
// ships as a file rather than as an archive.
void mono_wasm_link_icu_shim(void);
int mono_wasm_load_icu_data(const void *p_data);
// From modules/mono/glue/js/godot_mono_glue.js. The pack's corebindings.c declares it the
// same way and registers it below; that file is not compiled here, because the rest of
// what it registers needs the whole .NET JavaScript host.
char16_t *mono_wasm_get_locale_info(const uint16_t *p_locale, int32_t p_locale_length, const uint16_t *p_culture,
		int32_t p_culture_length, const uint16_t *r_result, int32_t p_result_max_length, int *r_result_length);
// [JSImport]. Also from godot_mono_glue.js; corebindings.c declares these the same way.
char *godot_mono_get_local_timezone(void);
void *mono_wasm_bind_js_import_ST(void *p_signature);
void mono_wasm_invoke_jsimport_ST(int p_function_handle, void *p_args);
// Declared here rather than taken from mono-gc.h, which does not expose them to embedders.
int mono_gc_register_root(char *p_start, size_t p_size, void *p_descr, int p_source, void *p_key, const char *p_msg);
void mono_gc_deregister_root(char *p_addr);
}
#endif
#endif

GDMono *GDMono::singleton = nullptr;

namespace {
hostfxr_initialize_for_dotnet_command_line_fn hostfxr_initialize_for_dotnet_command_line = nullptr;
hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config = nullptr;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
hostfxr_close_fn hostfxr_close = nullptr;

#ifndef TOOLS_ENABLED
typedef int(CORECLR_DELEGATE_CALLTYPE *coreclr_create_delegate_fn)(void *hostHandle, unsigned int domainId, const char *entryPointAssemblyName, const char *entryPointTypeName, const char *entryPointMethodName, void **delegate);
typedef int(CORECLR_DELEGATE_CALLTYPE *coreclr_initialize_fn)(const char *exePath, const char *appDomainFriendlyName, int propertyCount, const char **propertyKeys, const char **propertyValues, void **hostHandle, unsigned int *domainId);

coreclr_create_delegate_fn coreclr_create_delegate = nullptr;
coreclr_initialize_fn coreclr_initialize = nullptr;

#ifdef ANDROID_ENABLED
mono_install_assembly_preload_hook_fn mono_install_assembly_preload_hook = nullptr;
mono_assembly_name_get_name_fn mono_assembly_name_get_name = nullptr;
mono_assembly_name_get_culture_fn mono_assembly_name_get_culture = nullptr;
mono_image_open_from_data_with_name_fn mono_image_open_from_data_with_name = nullptr;
mono_assembly_load_from_full_fn mono_assembly_load_from_full = nullptr;
#endif
#endif

#ifdef _WIN32
static_assert(sizeof(char_t) == sizeof(char16_t));
using HostFxrCharString = Char16String;
#define HOSTFXR_STR(m_str) L##m_str
#else
static_assert(sizeof(char_t) == sizeof(char));
using HostFxrCharString = CharString;
#define HOSTFXR_STR(m_str) m_str
#endif

HostFxrCharString str_to_hostfxr(const String &p_str) {
#ifdef _WIN32
	return p_str.utf16();
#else
	return p_str.utf8();
#endif
}

const char_t *get_data(const HostFxrCharString &p_char_str) {
	return (const char_t *)p_char_str.get_data();
}

#ifdef TOOLS_ENABLED
bool try_get_dotnet_root_from_command_line(String &r_dotnet_root) {
	String pipe;
	List<String> args;
	args.push_back("--list-sdks");

	int exitcode;
	Error err = OS::get_singleton()->execute("dotnet", args, &pipe, &exitcode, true);

	ERR_FAIL_COND_V_MSG(err != OK, false, String(".NET failed to get list of installed SDKs. Error: ") + error_names[err]);
	ERR_FAIL_COND_V_MSG(exitcode != 0, false, pipe);

	Vector<String> sdks = pipe.strip_edges().replace("\r\n", "\n").split("\n", false);

	godotsharp::SemVerParser sem_ver_parser;

	godotsharp::SemVer latest_sdk_version;
	String latest_sdk_path;

	for (const String &sdk : sdks) {
		// The format of the SDK lines is:
		// 8.0.401 [/usr/share/dotnet/sdk]
		String version_string = sdk.get_slice(" ", 0);
		String path = sdk.get_slice(" ", 1);
		path = path.substr(1, path.length() - 2);

		godotsharp::SemVer version;
		if (!sem_ver_parser.parse(version_string, version)) {
			WARN_PRINT("Unable to parse .NET SDK version '" + version_string + "'.");
			continue;
		}

		if (!DirAccess::exists(path)) {
			WARN_PRINT("Found .NET SDK version '" + version_string + "' with invalid path '" + path + "'.");
			continue;
		}

		if (version > latest_sdk_version) {
			latest_sdk_version = version;
			latest_sdk_path = path;
		}
	}

	if (!latest_sdk_path.is_empty()) {
		print_verbose("Found .NET SDK at " + latest_sdk_path);
		// The `dotnet_root` is the parent directory.
		r_dotnet_root = latest_sdk_path.path_join("..").simplify_path();
		return true;
	}

	return false;
}
#endif

String find_hostfxr() {
#ifdef TOOLS_ENABLED
	String dotnet_root;
	String fxr_path;
	if (godotsharp::hostfxr_resolver::try_get_path(dotnet_root, fxr_path)) {
		return fxr_path;
	}

	// hostfxr_resolver doesn't look for dotnet in `PATH`. If it fails, we try to use the dotnet
	// executable in `PATH` to find the `dotnet_root` and get the `hostfxr_path` from there.
	if (try_get_dotnet_root_from_command_line(dotnet_root)) {
		if (godotsharp::hostfxr_resolver::try_get_path_from_dotnet_root(dotnet_root, fxr_path)) {
			return fxr_path;
		}
	}

	ERR_PRINT(String() + ".NET: One of the dependent libraries is missing. " +
			"Typically when the `hostfxr`, `hostpolicy` or `coreclr` dynamic " +
			"libraries are not present in the expected locations.");

	return String();
#else

#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("hostfxr.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libhostfxr.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libhostfxr.so");
#else
#error "Platform not supported (yet?)"
#endif

	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}

	return String();

#endif
}

#ifndef TOOLS_ENABLED
String find_monosgen() {
#if defined(ANDROID_ENABLED)
	// Android includes all native libraries in the libs directory of the APK
	// so we assume it exists and use only the name to dlopen it.
	return "libmonosgen-2.0.so";
#else
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("monosgen-2.0.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libmonosgen-2.0.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libmonosgen-2.0.so");
#else
#error "Platform not supported (yet?)"
#endif

	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}

	return String();
#endif
}

String find_coreclr() {
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("coreclr.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libcoreclr.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir()
								.path_join("libcoreclr.so");
#else
#error "Platform not supported (yet?)"
#endif

	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}

	return String();
}
#endif

bool load_hostfxr(void *&r_hostfxr_dll_handle) {
	String hostfxr_path = find_hostfxr();

	if (hostfxr_path.is_empty()) {
		return false;
	}

	print_verbose("Found hostfxr: " + hostfxr_path);

	Error err = OS::get_singleton()->open_dynamic_library(hostfxr_path, r_hostfxr_dll_handle);

	if (err != OK) {
		return false;
	}

	void *lib = r_hostfxr_dll_handle;

	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_dotnet_command_line", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_dotnet_command_line = (hostfxr_initialize_for_dotnet_command_line_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_runtime_config", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_get_runtime_delegate", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_close", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_close = (hostfxr_close_fn)symbol;

	return (hostfxr_initialize_for_runtime_config &&
			hostfxr_get_runtime_delegate &&
			hostfxr_close);
}

#ifndef TOOLS_ENABLED
bool load_coreclr(void *&r_coreclr_dll_handle) {
	String coreclr_path = find_coreclr();

	bool is_monovm = false;
	if (coreclr_path.is_empty()) {
		// Fallback to MonoVM (should have the same API as CoreCLR).
		coreclr_path = find_monosgen();
		is_monovm = true;
	}

	if (coreclr_path.is_empty()) {
		return false;
	}

	const String coreclr_name = is_monovm ? "monosgen" : "coreclr";
	print_verbose("Found " + coreclr_name + ": " + coreclr_path);

	Error err = OS::get_singleton()->open_dynamic_library(coreclr_path, r_coreclr_dll_handle);

	if (err != OK) {
		return false;
	}

	void *lib = r_coreclr_dll_handle;

	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_initialize", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	coreclr_initialize = (coreclr_initialize_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_create_delegate", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	coreclr_create_delegate = (coreclr_create_delegate_fn)symbol;

#ifdef ANDROID_ENABLED
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_install_assembly_preload_hook", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	mono_install_assembly_preload_hook = (mono_install_assembly_preload_hook_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_name_get_name", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	mono_assembly_name_get_name = (mono_assembly_name_get_name_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_name_get_culture", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	mono_assembly_name_get_culture = (mono_assembly_name_get_culture_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_image_open_from_data_with_name", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	mono_image_open_from_data_with_name = (mono_image_open_from_data_with_name_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_load_from_full", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	mono_assembly_load_from_full = (mono_assembly_load_from_full_fn)symbol;
#endif

	return (coreclr_initialize &&
			coreclr_create_delegate);
}
#endif

#ifdef TOOLS_ENABLED
load_assembly_and_get_function_pointer_fn initialize_hostfxr_for_config(const char_t *p_config_path) {
	hostfxr_handle cxt = nullptr;
	int rc = hostfxr_initialize_for_runtime_config(p_config_path, nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_runtime_config failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;

	rc = hostfxr_get_runtime_delegate(cxt,
			hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);

	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#else
load_assembly_and_get_function_pointer_fn initialize_hostfxr_self_contained(
		const char_t *p_main_assembly_path) {
	hostfxr_handle cxt = nullptr;

	List<String> cmdline_args = OS::get_singleton()->get_cmdline_args();

	List<HostFxrCharString> argv_store;
	Vector<const char_t *> argv;
	argv.resize(cmdline_args.size() + 1);

	argv.write[0] = p_main_assembly_path;

	int i = 1;
	for (const String &E : cmdline_args) {
		HostFxrCharString &stored = argv_store.push_back(str_to_hostfxr(E))->get();
		argv.write[i] = get_data(stored);
		i++;
	}

	int rc = hostfxr_initialize_for_dotnet_command_line(argv.size(), argv.ptrw(), nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_dotnet_command_line failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;

	rc = hostfxr_get_runtime_delegate(cxt,
			hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);

	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#endif

#ifdef GD_MONO_PLUGIN_CALLBACKS
using godot_plugins_initialize_fn = bool (*)(void *, bool, gdmono::PluginCallbacks *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#else
using godot_plugins_initialize_fn = bool (*)(void *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#endif

#ifdef TOOLS_ENABLED
godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	HostFxrCharString godot_plugins_path = str_to_hostfxr(
			GodotSharpDirs::get_api_assemblies_dir().path_join("GodotPlugins.dll"));

	HostFxrCharString config_path = str_to_hostfxr(
			GodotSharpDirs::get_api_assemblies_dir().path_join("GodotPlugins.runtimeconfig.json"));

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_for_config(get_data(config_path));

	if (load_assembly_and_get_function_pointer == nullptr) {
		// Show a message box to the user to make the problem explicit (and explain a potential crash).
		OS::get_singleton()->alert(TTR("Unable to load .NET runtime, no compatible version was found.\nAttempting to create/edit a project will lead to a crash.\n\nPlease install the .NET SDK 8.0 or later from https://get.dot.net and restart Godot."), TTR("Failed to load .NET runtime"));
		ERR_FAIL_V_MSG(nullptr, ".NET: Failed to load compatible .NET runtime");
	}

	r_runtime_initialized = true;

	print_verbose(".NET: hostfxr initialized");

	int rc = load_assembly_and_get_function_pointer(get_data(godot_plugins_path),
			HOSTFXR_STR("GodotPlugins.Main, GodotPlugins"),
			HOSTFXR_STR("InitializeFromEngine"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
#else
godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	String assembly_name = Path::get_csharp_project_name();

	HostFxrCharString assembly_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir()
					.path_join(assembly_name + ".dll"));

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_self_contained(get_data(assembly_path));
	ERR_FAIL_NULL_V(load_assembly_and_get_function_pointer, nullptr);

	r_runtime_initialized = true;

	print_verbose(".NET: hostfxr initialized");

	int rc = load_assembly_and_get_function_pointer(get_data(assembly_path),
			get_data(str_to_hostfxr("GodotPlugins.Game.Main, " + assembly_name)),
			HOSTFXR_STR("InitializeFromGameProject"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}

godot_plugins_initialize_fn try_load_native_aot_library(void *&r_aot_dll_handle) {
	String assembly_name = Path::get_csharp_project_name();

#if defined(WINDOWS_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dll");
#elif defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dylib");
#elif defined(ANDROID_ENABLED)
	String native_aot_so_path = "lib" + assembly_name + ".so";
#elif defined(UNIX_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".so");
#else
#error "Platform not supported (yet?)"
#endif

	Error err = OS::get_singleton()->open_dynamic_library(native_aot_so_path, r_aot_dll_handle);

	if (err != OK) {
		return nullptr;
	}

	void *lib = r_aot_dll_handle;

	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "godotsharp_game_main_init", symbol);
	ERR_FAIL_COND_V(err != OK, nullptr);
	return (godot_plugins_initialize_fn)symbol;
}
#endif

#ifndef TOOLS_ENABLED
#ifdef ANDROID_ENABLED
MonoAssembly *load_assembly_from_pck(MonoAssemblyName *p_assembly_name, char **p_assemblies_path, void *p_user_data) {
	constexpr bool ref_only = false;

	const char *name = mono_assembly_name_get_name(p_assembly_name);
	const char *culture = mono_assembly_name_get_culture(p_assembly_name);

	String assembly_name;
	if (culture && strcmp(culture, "")) {
		assembly_name += culture;
		assembly_name += "/";
	}
	assembly_name += name;
	if (!assembly_name.ends_with(".dll")) {
		assembly_name += ".dll";
	}

	String path = GodotSharpDirs::get_api_assemblies_dir();
	path = path.path_join(assembly_name);

	print_verbose(".NET: Loading assembly '" + assembly_name + "' from '" + path + "'.");

	if (!FileAccess::exists(path)) {
		// We could not find the assembly, return null so another hook may find it.
		return nullptr;
	}

	Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
	ERR_FAIL_COND_V_MSG(data.is_empty(), nullptr, ".NET: Could not read assembly in '" + path + "'.");

	MonoImageOpenStatus status = MONO_IMAGE_OK;

	MonoImage *image = mono_image_open_from_data_with_name(
			reinterpret_cast<char *>(data.ptrw()), data.size(),
			/*need_copy*/ true,
			&status,
			ref_only,
			assembly_name.utf8().get_data());

	ERR_FAIL_COND_V_MSG(status != MONO_IMAGE_OK || image == nullptr, nullptr, ".NET: Failed to open assembly image.");

	status = MONO_IMAGE_OK;

	MonoAssembly *assembly = mono_assembly_load_from_full(
			image, assembly_name.utf8().get_data(),
			&status,
			ref_only);

	ERR_FAIL_COND_V_MSG(status != MONO_IMAGE_OK || assembly == nullptr, nullptr, ".NET: Failed to load assembly from image.");

	return assembly;
}
#endif

#ifdef WEB_ENABLED
void gd_mono_wasm_log(const char *p_domain, const char *p_level, const char *p_message, int p_fatal, void *p_user_data) {
	// mono_trace_set_log_handler asserts on a null callback, so this must exist.
	if (p_fatal) {
		// A fatal callback must not return; the runtime's invariants may no longer hold.
		ERR_PRINT(vformat(".NET: %s", p_message ? p_message : ""));
		CRASH_NOW_MSG(vformat(".NET: fatal runtime error: %s", p_message ? p_message : ""));
	} else {
		print_verbose(vformat(".NET: %s", p_message ? p_message : ""));
	}
}

// Where the managed payload is copied to before the runtime starts. See below for why a
// copy is needed at all.
const char *GD_MONO_WASM_ASSEMBLIES_DIR = "/godot-dotnet";

// The GC roots the JavaScript interop uses to keep managed objects alive across a call.
// driver.c defines these; that file is not compiled here, so they are defined here instead.
int gd_mono_wasm_register_root(char *p_start, size_t p_size, const char *p_name) {
	// MONO_ROOT_SOURCE_EXTERNAL, which mono-gc.h does not expose to embedders.
	const int root_source_external = 4;
	return mono_gc_register_root(p_start, p_size, nullptr, root_source_external, nullptr,
			p_name ? p_name : "mono_wasm_register_root");
}

void gd_mono_wasm_deregister_root(char *p_addr) {
	mono_gc_deregister_root(p_addr);
}

// System.Console.Clear on a target with no console to clear.
void gd_mono_wasm_console_clear() {
}

// The domain the runtime starts in, kept so strings can be created for the JavaScript
// side after startup.
MonoDomain *gd_mono_wasm_root_domain = nullptr;

// Hybrid globalization moves collation, casing and calendar data out of ICU and into the
// host, which is the mode .NET's own browser runtime runs in. The engine does not: it
// links ICU and turns the switch off, so nothing below is ever called.
//
// They are registered anyway. The interpreter resolves a method's internal calls when it
// prepares the method, not when a branch is taken, so preparing CompareInfo looks up the
// hybrid entry points even though the hybrid branch is dead -- and an unresolved one makes
// the runtime print that it is out of sync with its class libraries, which is alarming and
// says nothing about what happened. Answering with a real message costs nine small
// functions and turns a lie into an explanation.
//
// The caller frees the returned string, and expects a UTF-16 one, so plain malloc it is.
char16_t *gd_mono_wasm_hybrid_globalization_disabled() {
	static const char *message = "Hybrid globalization is not supported by the Godot Web export template, "
								 "which links ICU instead. This call should be unreachable.";
	size_t length = strlen(message);
	char16_t *utf16 = (char16_t *)malloc((length + 1) * sizeof(char16_t));
	ERR_FAIL_NULL_V(utf16, nullptr);
	for (size_t i = 0; i < length; i++) {
		utf16[i] = (char16_t)message[i];
	}
	utf16[length] = 0;
	return utf16;
}

char16_t *gd_mono_wasm_change_case(const uint16_t *, int32_t, const uint16_t *, int32_t, uint16_t *, int32_t, int32_t) {
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_compare_string(const uint16_t *, int32_t, const uint16_t *, int32_t, const uint16_t *, int32_t, int32_t, int32_t *r_result) {
	*r_result = -2;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_starts_with(const uint16_t *, int32_t, const uint16_t *, int32_t, const uint16_t *, int32_t, int32_t, int32_t *r_result) {
	*r_result = -2;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_ends_with(const uint16_t *, int32_t, const uint16_t *, int32_t, const uint16_t *, int32_t, int32_t, int32_t *r_result) {
	*r_result = -2;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_index_of(const uint16_t *, int32_t, const uint16_t *, int32_t, const uint16_t *, int32_t, int32_t, int32_t, int32_t *r_result) {
	*r_result = -2;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_get_calendar_info(const uint16_t *, int32_t, int32_t, const uint16_t *, int32_t, int32_t *r_length) {
	*r_length = -1;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_get_culture_info(const uint16_t *, int32_t, const uint16_t *, int32_t, int32_t *r_length) {
	*r_length = -1;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_get_first_day_of_week(const uint16_t *, int32_t, int32_t *r_result) {
	*r_result = -1;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

char16_t *gd_mono_wasm_get_first_week_of_year(const uint16_t *, int32_t, int32_t *r_result) {
	*r_result = -1;
	return gd_mono_wasm_hybrid_globalization_disabled();
}

// Copies the published payload out of the pck and into Emscripten's in-memory filesystem,
// appending every assembly it finds to a TRUSTED_PLATFORM_ASSEMBLIES list.
//
// Reading straight from the pck through an assembly preload hook, as Android does, does
// not survive contact with the rest of .NET: a preload hook is only consulted when
// resolving an assembly *name*, and both the entry point here and GodotPlugins' own
// PluginLoadContext work from paths. Neither goes through the hook, and neither
// understands res://. Giving the runtime real files it can open is what every other .NET
// host does, and it costs one copy of the managed payload.
//
// The whole tree is copied, not just the assemblies: satellite assemblies live in
// per-culture subdirectories, and a project can publish content files that managed code
// then expects to find relative to AppContext.BaseDirectory.
Error gd_mono_wasm_copy_publish_tree(const String &p_source_dir, const String &p_target_dir, bool p_is_root, String &r_trusted_platform_assemblies) {
	Ref<DirAccess> dir = DirAccess::open(p_source_dir);
	ERR_FAIL_COND_V_MSG(dir.is_null(), ERR_CANT_OPEN, ".NET: Could not open directory: " + p_source_dir);

	Error mkdir_err = DirAccess::make_dir_recursive_absolute(p_target_dir);
	ERR_FAIL_COND_V_MSG(mkdir_err != OK && mkdir_err != ERR_ALREADY_EXISTS, mkdir_err,
			".NET: Could not create directory: " + p_target_dir);

	dir->list_dir_begin();
	for (String file_name = dir->get_next(); !file_name.is_empty(); file_name = dir->get_next()) {
		if (file_name == "." || file_name == "..") {
			continue;
		}

		String source_path = p_source_dir.path_join(file_name);
		String target_path = p_target_dir.path_join(file_name);

		// Every failure below is fatal rather than skipped. A browser can run out of heap
		// part way through copying the payload, and carrying on would start the runtime
		// against an incomplete one, which surfaces later as something unrelated.
		if (dir->current_is_dir()) {
			Error sub_err = gd_mono_wasm_copy_publish_tree(source_path, target_path, false, r_trusted_platform_assemblies);
			if (sub_err != OK) {
				dir->list_dir_end();
				return sub_err;
			}
			continue;
		}

		// Opened rather than read through get_file_as_bytes, which cannot tell an empty
		// file from an unreadable one and would report every zero-byte file as an error.
		Ref<FileAccess> source = FileAccess::open(source_path, FileAccess::READ);
		if (source.is_null()) {
			dir->list_dir_end();
			ERR_FAIL_V_MSG(ERR_CANT_OPEN, ".NET: Could not read: " + source_path);
		}
		uint64_t source_length = source->get_length();
		Vector<uint8_t> data = source->get_buffer(source_length);
		source->close();
		// get_buffer returns what it managed to read, so a decompression failure or a heap
		// that could not hold the file comes back as a shorter vector rather than an error.
		// Writing that would hand the runtime a truncated assembly.
		if ((uint64_t)data.size() != source_length) {
			dir->list_dir_end();
			ERR_FAIL_V_MSG(ERR_FILE_CANT_READ, ".NET: Could not read all of: " + source_path);
		}

		Ref<FileAccess> target = FileAccess::open(target_path, FileAccess::WRITE);
		if (target.is_null()) {
			dir->list_dir_end();
			ERR_FAIL_V_MSG(ERR_FILE_CANT_WRITE, ".NET: Could not write: " + target_path);
		}
		target->store_buffer(data);
		Error write_err = target->get_error();
		target->close();
		if (write_err != OK) {
			dir->list_dir_end();
			ERR_FAIL_V_MSG(ERR_FILE_CANT_WRITE, ".NET: Could not write all of: " + target_path);
		}

		// Only the root assemblies belong on the list. Content files are found through
		// AppContext.BaseDirectory, and the subdirectories hold satellite assemblies, which
		// share a name across cultures -- TRUSTED_PLATFORM_ASSEMBLIES is keyed by assembly
		// name, so listing them would have them mask one another instead of being picked up
		// by the runtime's own culture probing.
		if (p_is_root && file_name.get_extension().to_lower() == "dll") {
			if (!r_trusted_platform_assemblies.is_empty()) {
				r_trusted_platform_assemblies += ":";
			}
			r_trusted_platform_assemblies += target_path;
		}
	}
	dir->list_dir_end();

	return OK;
}

String gd_mono_wasm_extract_assemblies() {
	String trusted_platform_assemblies;
	Error err = gd_mono_wasm_copy_publish_tree(
			GodotSharpDirs::get_api_assemblies_dir(), GD_MONO_WASM_ASSEMBLIES_DIR, true, trusted_platform_assemblies);
	ERR_FAIL_COND_V(err != OK, String());
	return trusted_platform_assemblies;
}

// The AppContext switches the published runtimeconfig.json files ask for. A normal .NET
// host reads these; this one is the host, so it reads them too. Anything unreadable is
// skipped with a warning rather than treated as fatal: a missing switch degrades one
// library's behavior, while refusing to start takes the whole game down.
HashMap<String, String> gd_mono_wasm_published_config_properties() {
	HashMap<String, String> properties;

	// Only the project's own, the way a .NET host reads only the application's. The publish
	// root can hold others -- a dependency that is itself executable, or a content file
	// copied along -- and merging those would let an unrelated file decide the runtime's
	// configuration, with directory order picking the winner.
	String project_name = Path::get_csharp_project_name();
	if (project_name.is_empty()) {
		return properties;
	}

	String path = String(GD_MONO_WASM_ASSEMBLIES_DIR).path_join(project_name + ".runtimeconfig.json");
	if (!FileAccess::exists(path)) {
		return properties;
	}

	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		WARN_PRINT(".NET: Could not read " + path + "; its runtime configuration is ignored.");
		return properties;
	}

	Ref<JSON> json;
	json.instantiate();
	if (json->parse(file->get_as_text()) != OK) {
		WARN_PRINT(vformat(".NET: %s is not valid JSON (%s at line %d); its runtime configuration is ignored.",
				path, json->get_error_message(), json->get_error_line()));
		return properties;
	}

	Variant data = json->get_data();
	if (data.get_type() != Variant::DICTIONARY) {
		return properties;
	}
	Variant runtime_options = ((Dictionary)data).get("runtimeOptions", Dictionary());
	if (runtime_options.get_type() != Variant::DICTIONARY) {
		return properties;
	}
	Variant config_properties = ((Dictionary)runtime_options).get("configProperties", Dictionary());
	if (config_properties.get_type() != Variant::DICTIONARY) {
		return properties;
	}

	for (const KeyValue<Variant, Variant> &E : (Dictionary)config_properties) {
		// The runtime reads every property as a string, and expects the spellings the .NET
		// host writes: lowercase for booleans, invariant for numbers.
		switch (E.value.get_type()) {
			case Variant::BOOL:
				properties[E.key] = ((bool)E.value) ? "true" : "false";
				break;
			case Variant::STRING:
			case Variant::INT:
			case Variant::FLOAT:
				properties[E.key] = E.value;
				break;
			default:
				WARN_PRINT(vformat(".NET: Ignoring runtime configuration property %s in %s: only booleans, "
								   "numbers and strings can be passed to the runtime.",
						String(E.key), path));
				break;
		}
	}

	return properties;
}

godot_plugins_initialize_fn initialize_wasm_mono_and_godot_plugins(bool &r_runtime_initialized) {
	String trusted_platform_assemblies = gd_mono_wasm_extract_assemblies();
	ERR_FAIL_COND_V_MSG(trusted_platform_assemblies.is_empty(), nullptr,
			".NET: No managed assemblies were found in " + GodotSharpDirs::get_api_assemblies_dir() + ".");

	// The project's own AppContext switches, read before anything decides on ICU: a
	// project that asked for invariant globalization should not pay to load ICU data that
	// is then ignored. Godot.NET.Sdk records that request here because the WebAssembly SDK
	// cannot be told about it directly.
	HashMap<String, String> properties = gd_mono_wasm_published_config_properties();
	const String *requested_invariant = properties.getptr("System.Globalization.Invariant");
	const bool project_wants_invariant = requested_invariant && *requested_invariant == "true";

	// Otherwise globalization follows the payload: a project that published ICU data gets
	// real cultures. The buffer has to outlive the call, since the runtime reads from it
	// for as long as it is up.
	static Vector<uint8_t> icu_data;
	String icu_path = String(GD_MONO_WASM_ASSEMBLIES_DIR).path_join("icudt.dat");
	if (project_wants_invariant) {
		print_verbose(".NET: The project asked for invariant globalization; ICU data is not loaded.");
	} else if (!FileAccess::exists(icu_path)) {
		// Publishing no ICU data at all is a project asking for invariant globalization, and
		// is silently honored below. Publishing some other ICU payload -- which is what
		// WasmIcuDataFileName does -- is a project asking for cultures and not getting them,
		// so it is called out rather than quietly downgraded.
		Ref<DirAccess> icu_dir = DirAccess::open(GD_MONO_WASM_ASSEMBLIES_DIR);
		if (icu_dir.is_valid()) {
			for (const String &file_name : icu_dir->get_files()) {
				if (file_name.begins_with("icudt") && file_name.get_extension().to_lower() == "dat") {
					ERR_PRINT(vformat(".NET: The published payload contains %s but not icudt.dat, which is the "
									  "only ICU data file the Web export template loads. Globalization will be "
									  "invariant. Leave WasmIcuDataFileName unset to publish icudt.dat.",
							file_name));
					break;
				}
			}
		}
	}
	if (!project_wants_invariant && FileAccess::exists(icu_path)) {
		Error icu_err = OK;
		icu_data = FileAccess::get_file_as_bytes(icu_path, &icu_err);
		if (icu_err != OK || icu_data.is_empty()) {
			ERR_PRINT(".NET: Could not read " + icu_path + "; continuing with invariant globalization.");
			icu_data.clear();
		} else {
			// Before the runtime starts, as in the pack's driver.c.
			mono_wasm_link_icu_shim();
			if (!mono_wasm_load_icu_data(icu_data.ptr())) {
				ERR_PRINT(".NET: The runtime rejected " + icu_path + "; continuing with invariant globalization.");
				icu_data.clear();
			}
		}
	}
	const bool invariant_globalization = project_wants_invariant || icu_data.is_empty();

	// The runtime is already in this module; what it needs is the app context, because
	// there is no host to supply it. BaseDirectory points at the managed content rather
	// than "/" (which is what .NET's own driver.c uses, owning the whole filesystem).
	//
	// Hybrid globalization is off deliberately. It is the mode .NET's own browser host
	// runs in, and it moves collation, casing and calendar data out of ICU and into
	// JavaScript -- a second, larger host contract, for data the linked ICU already has.
	//
	// `properties` already holds whatever the project set through
	// RuntimeHostConfigurationOption or runtimeconfig.template.json, which an ordinary .NET
	// host would read and this one has to read itself; without it, switches like
	// System.Text.Json.JsonSerializer.IsReflectionEnabledByDefault would silently keep
	// their defaults on Web and nowhere else. The entries below are set afterwards, so a
	// project cannot overwrite the ones the template depends on.
	properties["TRUSTED_PLATFORM_ASSEMBLIES"] = trusted_platform_assemblies;
	properties["APP_CONTEXT_BASE_DIRECTORY"] = String(GD_MONO_WASM_ASSEMBLIES_DIR).path_join("");
	properties["RUNTIME_IDENTIFIER"] = "browser-wasm";
	properties["System.Globalization.Invariant"] = invariant_globalization ? "true" : "false";
	properties["System.Globalization.Hybrid"] = "false";

	// monovm_initialize does not copy, so these have to outlive the call.
	LocalVector<CharString> key_storage;
	LocalVector<CharString> value_storage;
	LocalVector<const char *> keys;
	LocalVector<const char *> values;
	key_storage.reserve(properties.size());
	value_storage.reserve(properties.size());
	for (const KeyValue<String, String> &E : properties) {
		key_storage.push_back(E.key.utf8());
		value_storage.push_back(E.value.utf8());
	}
	for (uint32_t i = 0; i < key_storage.size(); i++) {
		keys.push_back(key_storage[i].get_data());
		values.push_back(value_storage[i].get_data());
	}
	int monovm_rc = monovm_initialize(keys.size(), keys.ptr(), values.ptr());
	ERR_FAIL_COND_V_MSG(monovm_rc != 0, nullptr, ".NET: monovm_initialize failed with code: " + itos(monovm_rc));

	// Before the timezone bundle is registered, and before anything can cache the zone:
	// the bundle supplies the data, TZ says which of it is local. Without it every export
	// runs in UTC no matter where the player is.
	char *local_timezone = godot_mono_get_local_timezone();
	if (local_timezone) {
		setenv("TZ", local_timezone, /* overwrite */ 1);
		print_verbose(vformat(".NET: local time zone: %s", local_timezone));
		free(local_timezone);
	} else {
		WARN_PRINT(".NET: The browser did not report a time zone; times will be UTC.");
	}

	// After monovm_initialize and before the runtime starts, as in driver.c. Gives
	// System.TimeZoneInfo real data instead of UTC only.
	mono_register_timezones_bundle();

	// Selects the interpreter and initializes the icall/ilgen tables. Without it Mono
	// aborts in mini.c, since WebAssembly has no JIT.
	MonoDomain *domain = mono_wasm_load_runtime_common(0, (void *)&gd_mono_wasm_log, "");
	ERR_FAIL_NULL_V_MSG(domain, nullptr, ".NET: Failed to initialize the Mono runtime.");
	gd_mono_wasm_root_domain = domain;

	r_runtime_initialized = true;
	print_verbose(".NET: Mono runtime initialized (interpreter, statically linked)");

	// What the other platforms get for free from hostfxr_initialize_for_dotnet_command_line.
	// Without it Environment.GetCommandLineArgs() returns nothing on Web, so arguments the
	// HTML shell passes through its engine configuration reach GDScript but not C#. Set
	// after the runtime is up, which is where the pack's own host sets it, and before
	// anything managed can read it. The leading entry is the main assembly, as it is there.
	{
		List<String> cmdline_args = OS::get_singleton()->get_cmdline_args();
		String project_name = Path::get_csharp_project_name();

		LocalVector<CharString> arg_storage;
		LocalVector<char *> argv;
		arg_storage.reserve(cmdline_args.size() + 1);
		arg_storage.push_back(String(GD_MONO_WASM_ASSEMBLIES_DIR)
						.path_join(project_name.is_empty() ? "GodotPlugins.dll" : project_name + ".dll")
						.utf8());
		for (const String &E : cmdline_args) {
			arg_storage.push_back(E.utf8());
		}
		for (uint32_t i = 0; i < arg_storage.size(); i++) {
			argv.push_back((char *)arg_storage[i].get_data());
		}
		mono_runtime_set_main_args(argv.size(), argv.ptr());
	}

	// CultureData.JSInitLocaleInfo asks the host for a culture's language and region
	// display names, which the ICU this links does not carry, and does so for every
	// culture it creates -- including the default one, on the first touch of
	// CultureInfo.CurrentCulture. Left unregistered, that reports the runtime and class
	// libraries as out of sync. Registered unconditionally: it costs nothing in invariant
	// mode, where nothing calls it.
	//
	// This is the whole of Interop/JsGlobalization; the rest are the hybrid entry points,
	// registered for the reason given where they are defined.
	mono_add_internal_call("Interop/JsGlobalization::GetLocaleInfo", (void *)mono_wasm_get_locale_info);
	mono_add_internal_call("Interop/JsGlobalization::ChangeCase", (void *)gd_mono_wasm_change_case);
	mono_add_internal_call("Interop/JsGlobalization::CompareString", (void *)gd_mono_wasm_compare_string);
	mono_add_internal_call("Interop/JsGlobalization::StartsWith", (void *)gd_mono_wasm_starts_with);
	mono_add_internal_call("Interop/JsGlobalization::EndsWith", (void *)gd_mono_wasm_ends_with);
	mono_add_internal_call("Interop/JsGlobalization::IndexOf", (void *)gd_mono_wasm_index_of);
	mono_add_internal_call("Interop/JsGlobalization::GetCalendarInfo", (void *)gd_mono_wasm_get_calendar_info);
	mono_add_internal_call("Interop/JsGlobalization::GetCultureInfo", (void *)gd_mono_wasm_get_culture_info);
	mono_add_internal_call("Interop/JsGlobalization::GetFirstDayOfWeek", (void *)gd_mono_wasm_get_first_day_of_week);
	mono_add_internal_call("Interop/JsGlobalization::GetFirstWeekOfYear", (void *)gd_mono_wasm_get_first_week_of_year);

	// Interop/Runtime is what [JSImport] and [JSExport] are built on. Only the two
	// single-threaded entry points do anything; the rest are the parts of .NET's browser
	// host this does not have, and they are registered so that preparing a method that
	// mentions them does not report the runtime as out of sync with its class libraries.
	mono_add_internal_call("Interop/Runtime::BindJSImportST", (void *)mono_wasm_bind_js_import_ST);
	mono_add_internal_call("Interop/Runtime::InvokeJSImportST", (void *)mono_wasm_invoke_jsimport_ST);
	mono_add_internal_call("Interop/Runtime::RegisterGCRoot", (void *)gd_mono_wasm_register_root);
	mono_add_internal_call("Interop/Runtime::DeregisterGCRoot", (void *)gd_mono_wasm_deregister_root);
	mono_add_internal_call("System.ConsolePal::Clear", (void *)gd_mono_wasm_console_clear);

	// GodotPlugins, not the game's assembly: entering managed code needs a trampoline
	// generated ahead of time from the assembly the entry point lives in, and the game's
	// assembly did not exist when this template was built. See GD_MONO_PLUGIN_CALLBACKS.
	String assembly_path = String(GD_MONO_WASM_ASSEMBLIES_DIR).path_join("GodotPlugins.dll");
	MonoAssembly *assembly = mono_domain_assembly_open(domain, assembly_path.utf8().get_data());
	ERR_FAIL_NULL_V_MSG(assembly, nullptr, ".NET: Failed to open assembly: " + assembly_path);

	MonoImage *image = mono_assembly_get_image(assembly);
	MonoClass *klass = mono_class_from_name(image, "GodotPlugins", "Main");
	ERR_FAIL_NULL_V_MSG(klass, nullptr, ".NET: Failed to find GodotPlugins.Main.");

	MonoMethodDesc *desc = mono_method_desc_new(":InitializeFromEngine", 0);
	MonoMethod *method = mono_method_desc_search_in_class(desc, klass);
	mono_method_desc_free(desc);
	ERR_FAIL_NULL_V_MSG(method, nullptr, ".NET: Failed to find InitializeFromEngine.");

	// [UnmanagedCallersOnly], so a plain function pointer works, matching what the other
	// initialization strategies produce. MonoError is an out-parameter, not an optional one.
	MonoError ftnptr_error;
	mono_error_init(&ftnptr_error);
	godot_plugins_initialize_fn godot_plugins_initialize =
			(godot_plugins_initialize_fn)mono_method_get_unmanaged_callers_only_ftnptr(method, &ftnptr_error);
	if (!mono_error_ok(&ftnptr_error)) {
		String message = String::utf8(mono_error_get_message(&ftnptr_error));
		mono_error_cleanup(&ftnptr_error);
		ERR_FAIL_V_MSG(nullptr, ".NET: Failed to get GodotPlugins initialization function pointer: " + message);
	}
	mono_error_cleanup(&ftnptr_error);
	ERR_FAIL_NULL_V_MSG(godot_plugins_initialize, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
#endif // WEB_ENABLED

godot_plugins_initialize_fn initialize_coreclr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	String assembly_name = Path::get_csharp_project_name();

#ifdef ANDROID_ENABLED
	// Android requires installing a preload hook to load assemblies from inside the APK,
	// other platforms can find the assemblies with the default lookup.
	if (mono_install_assembly_preload_hook != nullptr) {
		mono_install_assembly_preload_hook(&load_assembly_from_pck, nullptr);
	}
#endif

	void *coreclr_handle = nullptr;
	unsigned int domain_id = 0;
	int rc = coreclr_initialize(nullptr, nullptr, 0, nullptr, nullptr, &coreclr_handle, &domain_id);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to initialize CoreCLR.");

	r_runtime_initialized = true;

	print_verbose(".NET: CoreCLR initialized");

	coreclr_create_delegate(coreclr_handle, domain_id,
			assembly_name.utf8().get_data(),
			"GodotPlugins.Game.Main",
			"InitializeFromGameProject",
			(void **)&godot_plugins_initialize);
	ERR_FAIL_NULL_V_MSG(godot_plugins_initialize, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
#endif

} // namespace

#ifdef WEB_ENABLED
// Turns a UTF-16 buffer into a managed string, for godot_mono_glue.js.
//
// The exception slot of a [JSImport] argument buffer holds a MonoString, so the JavaScript
// side cannot fill it with a plain UTF-16 buffer of its own: the runtime would read the
// object header that is not there, taking two of the characters as the length. The pack
// exposes mono_wasm_string_from_utf16_ref for this, from the driver.c that is not compiled
// here, so the equivalent is provided instead.
extern "C" MonoString *godot_mono_wasm_string_from_utf16(const char16_t *p_chars, int p_length) {
	if (!gd_mono_wasm_root_domain || !p_chars || p_length < 0) {
		return nullptr;
	}
	return mono_string_new_utf16(gd_mono_wasm_root_domain, (const mono_unichar2 *)p_chars, p_length);
}

#endif // WEB_ENABLED

bool GDMono::should_initialize() {
#ifdef TOOLS_ENABLED
	// The editor always needs to initialize the .NET module for now.
	return true;
#else
	return OS::get_singleton()->has_feature("dotnet");
#endif
}

static bool _on_core_api_assembly_loaded() {
	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}

	bool debug;
#ifdef DEBUG_ENABLED
	debug = true;
#else
	debug = false;
#endif // DEBUG_ENABLED

	GDMonoCache::managed_callbacks.GD_OnCoreApiAssemblyLoaded(debug);

	return true;
}

void GDMono::initialize() {
	print_verbose(".NET: Initializing module...");

	_init_godot_api_hashes();

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#if !defined(APPLE_EMBEDDED_ENABLED)
	// Check that the .NET assemblies directory exists before trying to use it.
	if (!DirAccess::exists(GodotSharpDirs::get_api_assemblies_dir())) {
		OS::get_singleton()->alert(vformat(RTR("Unable to find the .NET assemblies directory.\nMake sure the '%s' directory exists and contains the .NET assemblies."), GodotSharpDirs::get_api_assemblies_dir()), RTR(".NET assemblies not found"));
		ERR_FAIL_MSG(".NET: Assemblies not found");
	}
#endif

#if defined(WEB_ENABLED) && !defined(TOOLS_ENABLED)
	// Linked into this module, so there is no hostfxr or shared library to look for.
	godot_plugins_initialize = initialize_wasm_mono_and_godot_plugins(runtime_initialized);
	ERR_FAIL_NULL(godot_plugins_initialize);
#else
	if (load_hostfxr(hostfxr_dll_handle)) {
		godot_plugins_initialize = initialize_hostfxr_and_godot_plugins(runtime_initialized);
		ERR_FAIL_NULL(godot_plugins_initialize);
	} else {
#if !defined(TOOLS_ENABLED)
		if (load_coreclr(coreclr_dll_handle)) {
			godot_plugins_initialize = initialize_coreclr_and_godot_plugins(runtime_initialized);
		} else {
			void *dll_handle = nullptr;
			godot_plugins_initialize = try_load_native_aot_library(dll_handle);
			if (godot_plugins_initialize != nullptr) {
				runtime_initialized = true;
			}
		}

		if (godot_plugins_initialize == nullptr) {
			ERR_FAIL_MSG(".NET: Failed to load hostfxr");
		}
#else

		// Show a message box to the user to make the problem explicit (and explain a potential crash).
		OS::get_singleton()->alert(TTR("Unable to load .NET runtime, specifically hostfxr.\nAttempting to create/edit a project will lead to a crash.\n\nPlease install the .NET SDK 8.0 or later from https://get.dot.net and restart Godot."), TTR("Failed to load .NET runtime"));
		ERR_FAIL_MSG(".NET: Failed to load hostfxr");
#endif
	}
#endif // WEB_ENABLED && !TOOLS_ENABLED

	int32_t interop_funcs_size = 0;
	const void **interop_funcs = godotsharp::get_runtime_interop_funcs(interop_funcs_size);

	GDMonoCache::ManagedCallbacks managed_callbacks{};

	void *godot_dll_handle = nullptr;

#if defined(UNIX_ENABLED) && !defined(MACOS_ENABLED) && !defined(APPLE_EMBEDDED_ENABLED)
	// Managed code can access it on its own on other platforms
	godot_dll_handle = dlopen(nullptr, RTLD_NOW);
#endif

#ifdef GD_MONO_PLUGIN_CALLBACKS
	gdmono::PluginCallbacks plugin_callbacks_res;
	bool init_ok = godot_plugins_initialize(godot_dll_handle,
#ifdef TOOLS_ENABLED
			Engine::get_singleton()->is_editor_hint(),
#else
			false,
#endif
			&plugin_callbacks_res, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	ERR_FAIL_COND_MSG(!init_ok, ".NET: GodotPlugins initialization failed");

	plugin_callbacks = plugin_callbacks_res;
#else
	bool init_ok = godot_plugins_initialize(godot_dll_handle, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	ERR_FAIL_COND_MSG(!init_ok, ".NET: GodotPlugins initialization failed");
#endif

	GDMonoCache::update_godot_api_cache(managed_callbacks);

	print_verbose(".NET: GodotPlugins initialized");

	_on_core_api_assembly_loaded();

#ifdef WEB_ENABLED
	// Not "try": an exported Web build only reaches here because the export carried a
	// managed payload, so a failure to load it is not the editor's "maybe it is not built
	// yet" case. Continuing would start the game with every C# script silently missing.
	ERR_FAIL_COND_MSG(!_load_project_assembly(), ".NET: Failed to load the project assembly");
#elif defined(GD_MONO_PLUGIN_CALLBACKS)
	_try_load_project_assembly();
#endif

	initialized = true;
}

#ifdef TOOLS_ENABLED
void GDMono::_try_load_project_assembly() {
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}

	// Load the project's main assembly. This doesn't necessarily need to succeed.
	// The game may not be using .NET at all, or if the project does use .NET and
	// we're running in the editor, it may just happen to be it wasn't built yet.
	if (!_load_project_assembly()) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			print_error(".NET: Failed to load project assembly");
		}
	}
}
#endif

void GDMono::_init_godot_api_hashes() {
#ifdef DEBUG_ENABLED
	get_api_core_hash();

#ifdef TOOLS_ENABLED
	get_api_editor_hash();
#endif // TOOLS_ENABLED
#endif // DEBUG_ENABLED
}

#ifdef DEBUG_ENABLED
uint64_t GDMono::get_api_core_hash() {
	if (api_core_hash == 0) {
		api_core_hash = ClassDB::get_api_hash(ClassDB::API_CORE);
	}
	return api_core_hash;
}
#ifdef TOOLS_ENABLED
uint64_t GDMono::get_api_editor_hash() {
	if (api_editor_hash == 0) {
		api_editor_hash = ClassDB::get_api_hash(ClassDB::API_EDITOR);
	}
	return api_editor_hash;
}
#endif // TOOLS_ENABLED
#endif // DEBUG_ENABLED

#ifdef GD_MONO_PLUGIN_CALLBACKS
bool GDMono::_load_project_assembly() {
	String assembly_name = Path::get_csharp_project_name();

#ifdef WEB_ENABLED
	// The copy extracted before the runtime started, not the pck: GodotPlugins resolves
	// dependencies with AssemblyDependencyResolver and opens files with File.Open, neither
	// of which knows about res://.
	String assembly_path = String(GD_MONO_WASM_ASSEMBLIES_DIR)
								   .path_join(assembly_name + ".dll");
#else
	String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir()
								   .path_join(assembly_name + ".dll");
	assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);
#endif

	if (!FileAccess::exists(assembly_path)) {
		return false;
	}

	String loaded_assembly_path;
	bool success = plugin_callbacks.LoadProjectAssemblyCallback(assembly_path.utf16().get_data(), &loaded_assembly_path);

	if (success) {
		project_assembly_path = loaded_assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(loaded_assembly_path);
	}

	return success;
}
#endif

#ifdef GD_MONO_HOT_RELOAD
void GDMono::reload_failure() {
	if (++project_load_failure_count >= (int)GLOBAL_GET("dotnet/project/assembly_reload_attempts")) {
		// After reloading a project has failed n times in a row, update the path and modification time
		// to stop any further attempts at loading this assembly, which probably is never going to work anyways.
		project_load_failure_count = 0;

		ERR_PRINT_ED(".NET: Giving up on assembly reloading. Please restart the editor if unloading was failing.");

		String assembly_name = Path::get_csharp_project_name();
		String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
		assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);
		project_assembly_path = assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(assembly_path);
	}
}

Error GDMono::reload_project_assemblies() {
	ERR_FAIL_COND_V(!runtime_initialized, ERR_BUG);

	finalizing_scripts_domain = true;

	if (!get_plugin_callbacks().UnloadProjectPluginCallback()) {
		ERR_PRINT_ED(".NET: Failed to unload assemblies. Please check https://github.com/godotengine/godot/issues/78513 for more information.");
		reload_failure();
		return FAILED;
	}

	finalizing_scripts_domain = false;

	// Load the project's main assembly. Here, during hot-reloading, we do
	// consider failing to load the project's main assembly to be an error.
	if (!_load_project_assembly()) {
		ERR_PRINT_ED(".NET: Failed to load project assembly.");
		reload_failure();
		return ERR_CANT_OPEN;
	}

	if (project_load_failure_count > 0) {
		project_load_failure_count = 0;
		ERR_PRINT_ED(".NET: Assembly reloading succeeded after failures.");
	}

	return OK;
}
#endif

GDMono::GDMono() {
	singleton = this;
}

GDMono::~GDMono() {
	finalizing_scripts_domain = true;

	if (hostfxr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(hostfxr_dll_handle);
	}
	if (coreclr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(coreclr_dll_handle);
	}

	finalizing_scripts_domain = false;
	runtime_initialized = false;

	singleton = nullptr;
}

namespace MonoBind {

GodotSharp *GodotSharp::singleton = nullptr;

void GodotSharp::reload_assemblies() {
#ifdef GD_MONO_HOT_RELOAD
	CRASH_COND(CSharpLanguage::get_singleton() == nullptr);
	// This method may be called more than once with `call_deferred`, so we need to check
	// again if reloading is needed to avoid reloading multiple times unnecessarily.
	if (CSharpLanguage::get_singleton()->is_assembly_reloading_needed()) {
		CSharpLanguage::get_singleton()->reload_assemblies();
	}
#endif
}

GodotSharp::GodotSharp() {
	singleton = this;
}

GodotSharp::~GodotSharp() {
	singleton = nullptr;
}

} // namespace MonoBind
