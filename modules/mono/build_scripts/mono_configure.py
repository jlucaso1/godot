import os
import re
import subprocess
import sys


def is_desktop(platform):
    return platform in ["windows", "macos", "linuxbsd"]


def is_unix_like(platform):
    return platform in ["macos", "linuxbsd", "android", "ios"]


def module_supports_tools_on(platform):
    return is_desktop(platform)


# Linked into the engine module. Order matters for wasm-ld. The -eh- and -simd variants
# are picked below, since they must match how the engine itself is compiled.
WASM_RUNTIME_LIBS = [
    "libmonosgen-2.0.a",
    "libmono-ee-interp.a",
    "libmono-icall-table.a",
    "libmono-component-debugger-stub-static.a",
    "libmono-component-diagnostics_tracing-stub-static.a",
    "libmono-component-hot_reload-stub-static.a",
    "libmono-component-marshal-ilgen-static.a",
    "libSystem.Native.a",
    "libSystem.Globalization.Native.a",
    "libSystem.IO.Compression.Native.a",
    "libicuuc.a",
    "libicui18n.a",
    "libicudata.a",
    "libz.a",
    # Supplies mono_register_timezones_bundle, so System.TimeZoneInfo has real data
    # instead of UTC only.
    "wasm-bundled-timezones.a",
]

# Compiled here rather than taken prebuilt, because they depend on the generated tables.
WASM_RUNTIME_SOURCES = ["runtime.c", "pinvoke.c"]


def _pinned_runtime_version():
    """The runtime pack version exported projects are published against.

    Single-sourced in WebRuntimeVersion.props, which Godot.NET.Sdk also reads. Anchored to
    this file for the same reason as the helper project below.
    """
    props = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "WebRuntimeVersion.props")
    try:
        with open(props, encoding="utf-8") as f:
            match = re.search(r"<GodotWebRuntimeVersion>([^<]+)</GodotWebRuntimeVersion>", f.read())
    except OSError:
        return ""
    return match.group(1).strip() if match else ""


def _runtime_version_from_pack(pack_dir):
    """The servicing version a runtime pack directory belongs to, or "" if not recognizable.

    Packs live at .../Microsoft.NETCore.App.Runtime.Mono.browser-wasm/<version>/runtimes/...

    Matched case-insensitively because the casing depends on where the pack was restored
    from: the workload directory keeps the package ID's own casing, while NuGet's
    global-packages cache lowercases it. A case-sensitive match sees only the first, which
    would leave the version check below silently passing for the other.
    """
    match = re.search(
        r"Microsoft\.NETCore\.App\.Runtime\.Mono[^/\\]*[/\\]([^/\\]+)[/\\]runtimes", pack_dir, re.IGNORECASE
    )
    return match.group(1) if match else ""


def _pack_dir_from_tables(tables_dir):
    """Recover the runtime pack the interop tables were generated against.

    `dotnet publish` writes an emcc-link.rsp next to the tables holding absolute paths
    into the pack it used, so the pairing is recoverable from the build output itself.
    """
    rsp = os.path.join(tables_dir, "emcc-link.rsp")
    if not os.path.isfile(rsp):
        return ""
    marker = os.path.join("runtimes", "browser-wasm", "native")
    with open(rsp, encoding="utf-8") as f:
        for match in re.finditer(r'"([^"]+)"', f.read()):
            path = match.group(1)
            index = path.find(marker)
            if index != -1:
                return path[: index + len(marker)]
    return ""


def _godot_api_assemblies_dir():
    """Locate Godot's own managed assemblies, which the interop tables have to cover.

    Without them, PInvokeTableGenerator emits no trampoline for
    GodotPlugins.Main::InitializeFromEngine, and the template links and runs right up to
    the point where it cannot enter managed code. So this is fatal rather than optional.

    They come from `build_assemblies.py`, which needs glue that only an editor build can
    generate, so a fresh checkout has to produce those first.
    """
    api_dir = os.environ.get("GODOT_DOTNET_API_DIR", "")
    if not api_dir:
        repo_root = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir, os.pardir)
        api_dir = os.path.join(repo_root, "bin", "GodotSharp", "Api", "Release")

    missing = [
        name for name in ("GodotSharp.dll", "GodotPlugins.dll") if not os.path.isfile(os.path.join(api_dir, name))
    ]
    if missing:
        print(
            f"Could not find {' and '.join(missing)} in {api_dir}.\n"
            "The Web template enters managed code through GodotPlugins.Main, and WebAssembly\n"
            "needs a trampoline generated ahead of time from the assembly holding it, so these\n"
            "must exist before the interop tables are generated. Build them with:\n"
            "  scons platform=<host> target=editor module_mono_enabled=yes\n"
            "  bin/godot.<host>.editor.<arch>.mono --headless --generate-mono-glue modules/mono/glue\n"
            "  python modules/mono/build_scripts/build_assemblies.py --godot-output-dir=./bin\n"
            "or set GODOT_DOTNET_API_DIR to a directory that already has them."
        )
        sys.exit(255)

    return os.path.abspath(api_dir)


def _find_wasm_runtime_pack():
    """Locate the generated interop tables and the runtime pack matching them.

    The tables supply the static P/Invoke mappings that WebAssembly needs, since it
    cannot synthesize arbitrary-signature native calls at runtime. They are resolved
    together with the pack rather than independently: a mismatched pair links cleanly,
    then fails at the first managed call.
    """
    tables_dir = os.environ.get("GODOT_DOTNET_WASM_TABLES", "")

    if not tables_dir:
        # Publishing the helper project both resolves the runtime pack and runs
        # PInvokeTableGenerator over a known assembly closure.
        # Anchored to this file: SCons calls configure() with the working directory set
        # to the SConscript's, modules/mono, so a repository-relative path misses.
        helper = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, "runtime", "GetRuntimePack")
        if os.path.isdir(helper):
            command = ["dotnet", "publish", helper, "-r", "browser-wasm", "--self-contained", "-c", "Release"]
            command.append("-p:GodotApiDir=" + _godot_api_assemblies_dir())
            try:
                # Captured rather than inherited so a successful publish stays quiet, then
                # replayed on failure: the SDK's own text names the cause, where
                # CalledProcessError only reports an exit status.
                subprocess.run(
                    command,
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
            except subprocess.CalledProcessError as exc:
                # Reported here, or the generic "pack not found" below blames the wrong thing.
                print(f"Failed to publish {helper}: {exc}\n{exc.output}")
                return "", ""
            except (FileNotFoundError, OSError) as exc:
                print(f"Failed to run `dotnet publish {helper}`: {exc}")
                return "", ""
            generated = os.path.join(helper, "obj", "Release", "net9.0", "browser-wasm", "wasm", "for-publish")
            if os.path.isdir(generated):
                tables_dir = generated

    if not tables_dir:
        return "", ""

    pack_dir = _pack_dir_from_tables(tables_dir)

    # Externally supplied tables are held to the pinned version as well as to their own
    # pack. Godot.NET.Sdk publishes exported projects against GodotWebRuntimeVersion, so a
    # template linked against a different servicing version links cleanly and then reports
    # the runtime and class libraries as out of sync at the first managed call -- which is
    # the failure the pin exists to prevent.
    if os.environ.get("GODOT_DOTNET_WASM_TABLES", "") and pack_dir:
        pinned = _pinned_runtime_version()
        found = _runtime_version_from_pack(pack_dir)
        if pinned and not found:
            # Warned rather than refused. A pack laid out somewhere this cannot read a
            # version from is unusual but not wrong, and failing would block a build that
            # may be perfectly consistent; staying silent would let the drift this check
            # exists to catch through unremarked.
            print(
                f"Warning: the runtime pack at {pack_dir} does not carry a version in its\n"
                f"path, so it cannot be checked against the {pinned} that exported projects\n"
                "are published against. If they disagree, the first managed call reports the\n"
                "runtime and class libraries as out of sync."
            )
        if pinned and found and found != pinned:
            print(
                "GODOT_DOTNET_WASM_TABLES was generated against .NET runtime pack "
                f"{found}, but exported projects are published against {pinned}.\n"
                "Linking one and publishing the other fails at the first managed call.\n"
                f"Regenerate the tables against {pinned}, or update "
                "modules/mono/WebRuntimeVersion.props to match."
            )
            sys.exit(255)

    # Honored only if it is the pack the tables were generated against.
    override = os.environ.get("GODOT_DOTNET_WASM_RUNTIME_PACK", "")
    if override:
        # Fail closed: with no derived pairing there is nothing to check the override against.
        if not pack_dir:
            print(
                "GODOT_DOTNET_WASM_RUNTIME_PACK is set, but the runtime pack that generated\n"
                "the interop tables could not be determined from their emcc-link.rsp, so the\n"
                "two cannot be checked against each other.\n"
                "Regenerate the tables, or point GODOT_DOTNET_WASM_TABLES at a valid\n"
                "obj/.../wasm/for-publish directory."
            )
            sys.exit(255)
        # realpath, not normpath: the override may name the same directory relatively
        # or through a symlink.
        if os.path.realpath(override) != os.path.realpath(pack_dir):
            print(
                "GODOT_DOTNET_WASM_RUNTIME_PACK does not match the runtime pack the interop\n"
                f"tables were generated against.\n  configured: {override}\n  tables use: {pack_dir}\n"
                "Linking mismatched archives and tables builds successfully and then fails at the\n"
                "first managed call. Regenerate the tables against this pack, or unset the variable."
            )
            sys.exit(255)
        pack_dir = override

    return pack_dir, tables_dir


def _configure_web(env, env_mono):
    # Checked before resolving the runtime pack: that can shell out to `dotnet publish`,
    # and an unsupported option combination is rejected either way.
    if env["arch"] != "wasm32":
        print("The .NET `browser-wasm` runtime pack is wasm32-only; build with arch=wasm32.")
        sys.exit(255)

    if env["threads"]:
        print(
            "The .NET runtime's threaded WebAssembly flavor drives its worker threads from\n"
            "JavaScript: libmonosgen-2.0.a then needs mono_wasm_start_deputy_thread_async,\n"
            "mono_wasm_start_io_thread_async, mono_wasm_schedule_synchronization_context and\n"
            "the mono_wasm_pthread_on_pthread_* callbacks, which dotnet.runtime.js supplies and\n"
            "this engine does not. Linking against it fails with those symbols undefined.\n"
            "Building the 'mono' module for Web currently requires threads=no."
        )
        sys.exit(255)

    if env["dlink_enabled"]:
        print(
            "The .NET runtime pack archives are not position-independent, so they cannot go\n"
            "into the side module that dlink_enabled=yes produces.\n"
            "Building the 'mono' module for Web currently requires dlink_enabled=no."
        )
        sys.exit(255)

    pack_dir, tables_dir = _find_wasm_runtime_pack()

    if not pack_dir or not os.path.isdir(pack_dir):
        print(
            "The 'mono' module for the Web platform requires the .NET `browser-wasm` runtime pack.\n"
            "Install the `wasm-tools` workload (`dotnet workload install wasm-tools`), or set\n"
            "GODOT_DOTNET_WASM_RUNTIME_PACK to an unpacked\n"
            "Microsoft.NETCore.App.Runtime.Mono.browser-wasm/runtimes/browser-wasm/native directory."
        )
        sys.exit(255)

    pinvoke_table = os.path.join(tables_dir, "pinvoke-table.h") if tables_dir else ""
    if not pinvoke_table or not os.path.isfile(pinvoke_table):
        print(
            "Could not find the generated .NET interop tables (pinvoke-table.h).\n"
            "WebAssembly cannot build interp->native trampolines at runtime, so these must be\n"
            "generated ahead of time by `dotnet publish -r browser-wasm` with WasmBuildNative=true.\n"
            "This needs the WebAssembly workload; install it with\n"
            "`dotnet workload restore modules/mono/runtime/GetRuntimePack/GetRuntimePack.csproj`\n"
            "rather than by name, since an SDK newer than the project's target framework calls\n"
            "it wasm-tools-net9 instead of wasm-tools.\n"
            "Set GODOT_DOTNET_WASM_TABLES to the resulting obj/.../wasm/for-publish directory."
        )
        sys.exit(255)

    # Checked on the table itself rather than on the inputs that produced it, so that tables
    # supplied through GODOT_DOTNET_WASM_TABLES are held to the same standard. Without this
    # entry the template links and runs, and then cannot enter managed code at all.
    entry_point_trampoline = "GodotPlugins_Main_InitializeFromEngine"
    with open(pinvoke_table, encoding="utf-8") as f:
        if entry_point_trampoline not in f.read():
            print(
                f"The generated interop tables have no {entry_point_trampoline} trampoline.\n"
                "The Web template enters managed code through GodotPlugins.Main, and WebAssembly\n"
                "needs that trampoline generated ahead of time from the assembly holding it, so\n"
                "the tables have to be generated with Godot's managed assemblies in the closure.\n"
                f"Tables: {tables_dir}\n"
                "Regenerate them, or unset GODOT_DOTNET_WASM_TABLES to have the build do it."
            )
            sys.exit(255)

    # The pack is built with native wasm exceptions, so sources compiled against it must
    # agree. This must go on env_mono: modules/mono/SCsub clones it before calling us, so
    # env alone would never reach runtime.c, pinvoke.c or gd_mono.cpp.
    env_mono.Append(CCFLAGS=["-fwasm-exceptions"])
    env.Append(LINKFLAGS=["-fwasm-exceptions"])

    # Interpreter-only: there is no JIT on wasm. Matches the runtime pack's own build.
    #
    # Deliberately not LINK_ICALLS: that swaps mono's built-in icall table for the
    # generated icall-table.h, which only covers the assemblies GetRuntimePack itself
    # publishes. A game's assemblies are not that set, so every icall outside it misses
    # the table and mono reports "runtime and class libraries are out of sync". Leaving it
    # off makes runtime.c call mono_icall_table_init() instead, which uses the complete
    # table from libmono-icall-table.a and does not depend on any assembly closure.
    #
    # GEN_PINVOKE has no such escape hatch -- the pack ships no pinvoke-tables-default.h,
    # so the P/Invoke table must be generated, and GetRuntimePack publishes untrimmed to
    # make it span the whole framework.
    #
    # INVARIANT_TIMEZONE and INVARIANT_GLOBALIZATION are deliberately absent: each only
    # guards a call in the pack's driver.c, which this does not compile. gd_mono.cpp makes
    # both calls itself -- the timezone bundle unconditionally, the ICU shim when the
    # exported project published ICU data.
    env_mono.Append(
        CPPDEFINES=[
            "GD_MONO_WASM_STATIC",
            "GEN_PINVOKE=1",
            "DISABLE_PERFTRACING_LISTEN_PORTS=1",
        ]
    )

    include_dirs = [
        tables_dir,
        os.path.join(pack_dir, "include", "mono-2.0"),
        os.path.join(pack_dir, "include", "wasm"),
    ]
    env_mono.Append(CPPPATH=include_dirs)

    # The object target is named explicitly: these sources live inside the NuGet pack,
    # and SCons would otherwise write objects into it.
    src_dir = os.path.join(pack_dir, "src")
    for source in WASM_RUNTIME_SOURCES:
        path = os.path.join(src_dir, source)
        if not os.path.isfile(path):
            print(f"Missing expected .NET runtime pack source: {path}")
            sys.exit(255)
        obj = env_mono.Object(target="dotnet_runtime_" + os.path.splitext(source)[0], source=path)
        env.modules_sources.append(obj)

    # wasm_simd controls -msimd128 on Godot's own objects; the Mono archive must match.
    eh_lib = "libmono-wasm-eh-wasm.a"
    simd_lib = "libmono-wasm-simd.a" if env["wasm_simd"] else "libmono-wasm-nosimd.a"

    for lib in WASM_RUNTIME_LIBS + [simd_lib, eh_lib]:
        path = os.path.join(pack_dir, lib)
        if not os.path.isfile(path):
            print(f"Missing expected .NET runtime pack archive: {path}")
            sys.exit(255)
        env.Append(LINKFLAGS=[path])

    # The interpreter needs a deep stack. platform/web already emits -sSTACK_SIZE from
    # stack_size, so enforce a floor rather than appending a second, overriding flag.
    MIN_STACK_SIZE_KIB = 5120
    if int(env["stack_size"]) < MIN_STACK_SIZE_KIB:
        print(
            f"The Mono interpreter needs at least {MIN_STACK_SIZE_KIB} KiB of stack; "
            f"stack_size is {env['stack_size']} KiB.\n"
            f"Build with stack_size={MIN_STACK_SIZE_KIB} or higher."
        )
        sys.exit(255)

    # The JS glue calls these from setTimeout closures, which the linker cannot see, so
    # they must be exported explicitly or the first timer raises a ReferenceError.
    # godot_mono_wasm_string_from_utf16 is called from the glue rather than from the engine,
    # so nothing in the C++ refers to it and the linker would drop it.
    env["EXPORTED_FUNCTIONS"] += [
        "_mono_background_exec",
        "_mono_wasm_execute_timer",
        "_godot_mono_wasm_string_from_utf16",
    ]

    # Required by the runtime pack; a mismatch surfaces as unrelated-looking failures.
    env.Append(
        LINKFLAGS=[
            "-sWASM_BIGINT=1",
            "-sALLOW_TABLE_GROWTH=1",
        ]
    )

    # "#" anchors at the repository root, not the current SConscript.
    env.AddJSLibraries(["#modules/mono/glue/js/godot_mono_glue.js"])


def configure(env, env_mono):
    # is_android = env["platform"] == "android"
    is_web = env["platform"] == "web"
    # is_ios = env["platform"] == "ios"
    # is_ios_sim = is_ios and env["arch"] in ["x86_32", "x86_64"]

    if env.editor_build:
        if not module_supports_tools_on(env["platform"]):
            raise RuntimeError("This module does not currently support building for this platform for editor builds.")
        env_mono.Append(CPPDEFINES=["GD_MONO_HOT_RELOAD"])

    if is_web:
        _configure_web(env, env_mono)
