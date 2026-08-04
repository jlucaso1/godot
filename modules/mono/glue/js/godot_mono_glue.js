/**************************************************************************/
/*  godot_mono_glue.js                                                    */
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

// When the Mono runtime is the wasm main module, .NET supplies these imports from
// `dotnet.native.js`. Godot is the main module instead, so the engine provides them.

// Exported by the statically linked runtime, so only the linker sees them. Declared here
// to keep .NET-specific symbols out of platform/web's shared ESLint config.
/* global _mono_background_exec, _mono_wasm_execute_timer, _godot_mono_wasm_string_from_utf16 */

// Emscripten inlines `$`-prefixed library members at their use sites, under the name
// without the `$`, so it never exists as a declaration for ESLint to find.
/* global GodotMonoGlobalization, GodotMonoJSImport */

const GodotMono = {
	// These call native code from deferred closures, which the linker cannot see. The
	// EXPORTED_FUNCTIONS entry in mono_configure.py is what keeps the bindings; these
	// record the dependency here too, so removing the export does not go unnoticed.
	schedule_background_exec__deps: ['mono_background_exec'],
	mono_wasm_schedule_timer__deps: ['mono_wasm_execute_timer'],

	// Mono schedules deferred work (finaliser runs, etc.) through this hook.
	schedule_background_exec: function () {
		setTimeout(function () {
			_mono_background_exec();
		}, 0);
	},

	// Backs System.Threading timers.
	mono_wasm_schedule_timer: function (shortestDueTimeMs) {
		setTimeout(function () {
			_mono_wasm_execute_timer();
		}, shortestDueTimeMs);
	},

	// Backs System.Security.Cryptography's RNG. Returns 0 on success, -1 if unavailable.
	// Existing prototypes leave this a stub, which is why crypto is broken there.
	mono_wasm_browser_entropy: function (bufferPtr, bufferLength) {
		const glob = (typeof globalThis !== 'undefined') ? globalThis : self;
		const crypto = glob.crypto || glob.msCrypto;
		if (!crypto || typeof crypto.getRandomValues !== 'function') {
			return -1;
		}
		const view = HEAPU8.subarray(bufferPtr, bufferPtr + bufferLength);
		// getRandomValues rejects requests larger than 65536 bytes.
		for (let offset = 0; offset < bufferLength; offset += 65536) {
			crypto.getRandomValues(view.subarray(offset, Math.min(offset + 65536, bufferLength)));
		}
		return 0;
	},

	// The jiterpreter is Mono's JavaScript-side JIT, which compiles hot interpreter traces
	// to WebAssembly at run time. It is not wired up here; the interpreter runs everything.
	//
	// The return values are a contract, not a formality. Anything other than these two
	// sentinels is taken as an index into the function table the jiterpreter would have
	// filled, and the interpreter calls it -- which traps as "null function or function
	// signature mismatch" the first time a method gets hot enough to be offered for
	// compilation, somewhere unrelated to whatever the game was doing.
	mono_interp_tier_prepare_jiterpreter: function () {
		// 1 is "not jitted". This is what dotnet.runtime.js returns when traces are off.
		return 1;
	},
	mono_interp_jit_wasm_entry_trampoline: function () {
		// 0 is "no trampoline"; the interpreter enters the method itself.
		return 0;
	},
	mono_interp_record_interp_entry: function () {},
	// Registers a compiled thunk for a JIT call. Registering none leaves the interpreter
	// on its generic path, and the invoke below is then never reached.
	mono_interp_jit_wasm_jit_call_trampoline: function () {},
	mono_interp_invoke_wasm_jit_call_trampoline: function () {},
	mono_interp_flush_jitcall_queue: function () {},
	mono_jiterp_free_method_data_js: function () {},

	// Shared by the globalization entry point below. Named members of a `$`-prefixed
	// object so emscripten inlines the object rather than emitting it as a symbol.
	$GodotMonoGlobalization__deps: ['malloc'],
	$GodotMonoGlobalization: {
		utf16ToString: function (start, end) {
			let str = '';
			for (let ptr = start; ptr < end; ptr += 2) {
				str += String.fromCharCode(HEAPU16[ptr >> 1]);
			}
			return str;
		},

		// The caller sizes the buffer, so a longer string is truncated rather than
		// allowed to run past the end.
		stringToUTF16: function (dstStart, dstEnd, str) {
			for (let i = 0; i < str.length && dstStart + i * 2 < dstEnd; i++) {
				HEAPU16[(dstStart >> 1) + i] = str.charCodeAt(i);
			}
		},

		// What a caught value should be reported as, for a value that is not necessarily an
		// Error. `throw null` and `throw undefined` are legal, and `err.toString()` on
		// either throws a TypeError out of the catch block, so the original exception
		// escapes across the interop boundary as a raw JavaScript throw rather than
		// becoming the managed exception the caller is waiting for. String() answers those
		// two, and is itself wrapped because an object with a null prototype or a throwing
		// toString defeats it as well.
		describeError: function (err) {
			try {
				return String(err);
			} catch (conversionError) {
				return 'A value that cannot be converted to a string was thrown.';
			}
		},

		// Returns a NUL-terminated UTF-16 string the runtime frees.
		stringToUTF16Ptr: function (str) {
			const ptr = _malloc((str.length + 1) * 2);
			for (let i = 0; i < str.length; i++) {
				HEAPU16[(ptr >> 1) + i] = str.charCodeAt(i);
			}
			HEAPU16[(ptr >> 1) + str.length] = 0;
			return ptr;
		},

		// BCP 47, with the spellings .NET accepts folded in: "zh-CHS" and "zh-CHT" are
		// .NET's names for script subtags Intl only knows as "Hans" and "Hant", and .NET
		// takes "_" where BCP 47 wants "-". Returns undefined for a name Intl rejects,
		// which the caller treats as "not a locale I know" rather than as an error.
		normalizeLocale: function (name) {
			if (!name) {
				return undefined;
			}
			try {
				// toLowerCase, not toLocaleLowerCase: a tag is ASCII and must fold the same
				// way everywhere. Under a Turkish host locale the locale-sensitive one turns
				// "fi-FI" into "fi-fı", which Intl then rejects.
				let locale = name.toLowerCase();
				if (locale.includes('zh')) {
					locale = locale.replace('chs', 'HANS').replace('cht', 'HANT');
				}
				// Every separator, not just the first. The pack's own host replaces one, which
				// leaves a tag like "sr_Latn_RS" as "sr-Latn_RS" for Intl to reject, costing
				// the caller its language and region names. Deliberately more permissive than
				// the reference here: a tag with fewer than two underscores is unaffected.
				const canonical = Intl.getCanonicalLocales(locale.replace(/_/g, '-'));
				return canonical.length > 0 ? canonical[0] : undefined;
			} catch (err) {
				return undefined;
			}
		},
	},

	// CultureData.JSInitLocaleInfo calls this for every culture it creates, the default
	// one included, whichever globalization mode the runtime is in: ICU carries no
	// display names on this target, so the host has to answer. Without it, the first
	// touch of CultureInfo.CurrentCulture kills the runtime, which is why globalization
	// has to be left invariant until the engine provides it.
	//
	// `culture` names what is being described, `locale` the language to describe it in.
	// The answer is "<language>##<region>"; the return value is 0 on success or a UTF-16
	// message the runtime raises as an exception.
	// --- [JSImport] --------------------------------------------------------------
	//
	// The runtime hands over a signature blob, expects the host to resolve the JavaScript
	// function it names and to remember it under a handle, and then calls that handle with
	// a buffer of marshaler slots. Both layouts are fixed by the runtime:
	//
	//   signature   +0 version (must be 2)   +4 argument count   +8 handle
	//               +16/+20 name offset and length, relative to the blob
	//               +24/+28 module name offset and length
	//               slots from +32, one per argument, 32 bytes each, type in the first byte
	//
	//   arguments   32 bytes per slot; [0] is where an exception goes, [1] is the return
	//               value, and the arguments themselves start at [2]
	//
	// Only the types below are marshaled. Anything else is refused when the import is
	// bound, naming the type, rather than at the first call: a signature this cannot
	// marshal is a mistake to report early, and guessing at one would corrupt memory
	// quietly instead.
	$GodotMonoJSImport__deps: ['$GodotMonoGlobalization', 'malloc', 'free', 'godot_mono_wasm_string_from_utf16'],
	$GodotMonoJSImport: {
		// Handle 0 is reserved by the runtime, so entry 0 is never handed out.
		bound: [null],

		SIGNATURE_VERSION: 2,
		HEADER_SIZE: 32,
		SLOT_SIZE: 32,
		ARG_SIZE: 32,

		// From MarshalerType in System.Runtime.InteropServices.JavaScript.
		TYPE: {
			NONE: 0, VOID: 1, DISCARD: 2, BOOLEAN: 3, BYTE: 4, CHAR: 5, INT16: 6,
			INT32: 7, INT52: 8, BIGINT64: 9, DOUBLE: 10, SINGLE: 11, INTPTR: 12,
			STRING: 15, EXCEPTION: 16,
		},

		typeName: function (type) {
			const names = GodotMonoJSImport.TYPE;
			const found = Object.keys(names).find((key) => names[key] === type);
			return found || `type ${type}`;
		},

		// Reads a managed argument slot into a JavaScript value.
		readers: {
			3: (arg) => HEAPU8[arg] !== 0,
			4: (arg) => HEAPU8[arg],
			5: (arg) => String.fromCharCode(HEAPU16[arg >> 1]),
			6: (arg) => HEAP16[arg >> 1],
			7: (arg) => HEAP32[arg >> 2],
			// Int52 is a double carrying an integer the runtime guarantees fits.
			8: (arg) => HEAPF64[arg >> 3],
			// A JavaScript BigInt, which is what the runtime marshals a long as when the
			// signature asks for JSType.BigInt rather than the Int52 default.
			9: (arg) => HEAP64[arg >> 3],
			10: (arg) => HEAPF64[arg >> 3],
			11: (arg) => HEAPF32[arg >> 2],
			12: (arg) => HEAP32[arg >> 2],
		},

		// Writes a JavaScript value back into a slot, tagging it with its type.
		writers: {
			1: (arg) => {
				HEAPU8[arg + 12] = 1;
			},
			2: (arg) => {
				HEAPU8[arg + 12] = 2;
			},
			3: (arg, value) => {
				HEAPU8[arg] = value ? 1 : 0;
				HEAPU8[arg + 12] = 3;
			},
			4: (arg, value) => {
				HEAPU8[arg] = value;
				HEAPU8[arg + 12] = 4;
			},
			5: (arg, value) => {
				HEAPU16[arg >> 1] = typeof value === 'string' ? value.charCodeAt(0) : value;
				HEAPU8[arg + 12] = 5;
			},
			6: (arg, value) => {
				HEAP16[arg >> 1] = value;
				HEAPU8[arg + 12] = 6;
			},
			7: (arg, value) => {
				HEAP32[arg >> 2] = value;
				HEAPU8[arg + 12] = 7;
			},
			8: (arg, value) => {
				HEAPF64[arg >> 3] = value;
				HEAPU8[arg + 12] = 8;
			},
			9: (arg, value) => {
				HEAP64[arg >> 3] = typeof value === 'bigint' ? value : BigInt(value);
				HEAPU8[arg + 12] = 9;
			},
			10: (arg, value) => {
				HEAPF64[arg >> 3] = value;
				HEAPU8[arg + 12] = 10;
			},
			11: (arg, value) => {
				HEAPF32[arg >> 2] = value;
				HEAPU8[arg + 12] = 11;
			},
			12: (arg, value) => {
				HEAP32[arg >> 2] = value;
				HEAPU8[arg + 12] = 12;
			},
		},

		// Slot 0 of an argument buffer is the exception the runtime rethrows as a
		// JSException. The slot holds a MonoString, not a UTF-16 buffer: writing raw
		// characters there does not fail loudly, because the runtime reads the object
		// header that is not present, takes two of the characters as the length, and
		// produces a message running on for megabytes of heap past the text. So the
		// string is built by the runtime itself, through the engine.
		setException: function (args, message) {
			const buffer = GodotMonoGlobalization.stringToUTF16Ptr(message);
			try {
				const managed = _godot_mono_wasm_string_from_utf16(buffer, message.length);
				HEAP32[args >> 2] = managed;
				HEAPU8[args + 12] = this.TYPE.STRING;
			} finally {
				// The runtime copied it; nothing else refers to this.
				_free(buffer);
			}
		},

		readString: function (base, offsetAt, lengthAt) {
			const offset = HEAP32[(base + offsetAt) >> 2];
			if (offset === 0) {
				return null;
			}
			// A byte count, not a character count.
			const byteLength = HEAP32[(base + lengthAt) >> 2];
			return GodotMonoGlobalization.utf16ToString(base + offset, base + offset + byteLength);
		},

		// "globalThis.Math.abs" and friends, resolved the way the runtime documents.
		resolveFunction: function (name) {
			const parts = name.split('.');
			let scope = globalThis;
			if (parts[0] === 'globalThis') {
				parts.shift();
			}
			for (let i = 0; i < parts.length - 1; i++) {
				scope = scope[parts[i]];
				if (!scope) {
					throw new Error(`${parts[i]} not found while looking up ${name}`);
				}
			}
			const fn = scope[parts[parts.length - 1]];
			if (typeof fn !== 'function') {
				throw new Error(`${name} must be a Function but was ${typeof fn}`);
			}
			return fn.bind(scope);
		},
	},

	// TimeZoneInfo.Local reads TZ. Registering the bundled zone database gives the runtime
	// the data but not the answer to "which zone is this", so without this every Web export
	// runs in UTC however the browser is configured, while explicit lookups by id work --
	// which makes it look like timezones work when only half of them do.
	godot_mono_get_local_timezone__deps: ['$GodotMonoGlobalization', 'malloc'],
	godot_mono_get_local_timezone: function () {
		try {
			const zone = Intl.DateTimeFormat().resolvedOptions().timeZone;
			if (!zone) {
				return 0;
			}
			const bytes = lengthBytesUTF8(zone) + 1;
			const ptr = _malloc(bytes);
			stringToUTF8(zone, ptr, bytes);
			return ptr;
		} catch (err) {
			return 0;
		}
	},

	mono_wasm_bind_js_import_ST__deps: ['$GodotMonoJSImport', '$GodotMonoGlobalization', 'malloc'],
	mono_wasm_bind_js_import_ST: function (signature) {
		const js = GodotMonoJSImport;
		try {
			const version = HEAP32[signature >> 2];
			if (version !== js.SIGNATURE_VERSION) {
				throw new Error(`Signature version ${version} mismatch.`);
			}

			const name = js.readString(signature, 16, 20);
			const moduleName = js.readString(signature, 24, 28);
			if (moduleName) {
				// ES6 module imports need JSHost.ImportAsync, which needs the promise
				// machinery this does not have.
				throw new Error(`[JSImport] from an ES6 module ('${moduleName}') is not supported by the Godot Web export template.`);
			}
			const handle = HEAP32[(signature + 8) >> 2];
			const argCount = HEAP32[(signature + 4) >> 2];

			const slotType = (slotIndex) => HEAPU8[signature + js.HEADER_SIZE + slotIndex * js.SLOT_SIZE];

			const readers = [];
			for (let i = 0; i < argCount; i++) {
				const type = slotType(i + 2);
				const reader = js.readers[type];
				if (!reader) {
					throw new Error(`[JSImport] ${name}: argument ${i + 1} is ${js.typeName(type)}, which the Godot Web export template cannot marshal. Supported: bool, byte, char, short, int, long, float, double and nint.`);
				}
				readers.push(reader);
			}

			const returnType = slotType(1);
			const writer = js.writers[returnType];
			if (!writer) {
				throw new Error(`[JSImport] ${name}: the return value is ${js.typeName(returnType)}, which the Godot Web export template cannot marshal. Supported: void, bool, byte, char, short, int, long, float, double and nint.`);
			}
			const discardsResult = returnType === js.TYPE.VOID || returnType === js.TYPE.DISCARD;

			const fn = js.resolveFunction(name);
			js.bound[handle] = function (args) {
				const values = [];
				for (let i = 0; i < readers.length; i++) {
					values.push(readers[i](args + (i + 2) * js.ARG_SIZE));
				}
				const result = fn(...values);
				if (discardsResult) {
					writer(args + js.ARG_SIZE);
				} else {
					writer(args + js.ARG_SIZE, result);
				}
			};
			return 0;
		} catch (err) {
			// The runtime turns this into the JSException the [JSImport] call throws.
			return GodotMonoGlobalization.stringToUTF16Ptr(GodotMonoGlobalization.describeError(err));
		}
	},

	mono_wasm_invoke_jsimport_ST__deps: ['$GodotMonoJSImport', '$GodotMonoGlobalization', 'malloc'],
	mono_wasm_invoke_jsimport_ST: function (handle, args) {
		const js = GodotMonoJSImport;
		const bound = js.bound[handle];
		if (!bound) {
			js.setException(args, `Imported function handle expected ${handle}`);
			return;
		}
		try {
			bound(args);
		} catch (err) {
			js.setException(args, GodotMonoGlobalization.describeError(err));
		}
	},

	mono_wasm_get_locale_info__deps: ['$GodotMonoGlobalization', 'malloc'],
	mono_wasm_get_locale_info: function (locale, localeLength, culture, cultureLength, dst, dstMaxLength, dstLength) {
		const glue = GodotMonoGlobalization;
		let requested = '';
		// Both fallbacks hand the caller its own name back. The caller sized the buffer, so
		// this has to respect dstMaxLength exactly as the display-name path does; writing a
		// longer name would run past the end and then report the overrun as a success.
		const giveBackRequestedName = () => {
			if (requested.length > dstMaxLength) {
				throw new Error(`Culture name ${requested} exceeds length of ${dstMaxLength}.`);
			}
			glue.stringToUTF16(dst, dst + 2 * requested.length, requested);
			HEAP32[dstLength >> 2] = requested.length;
			return 0;
		};
		try {
			requested = glue.utf16ToString(culture, culture + 2 * cultureLength);
			const cultureName = glue.normalizeLocale(requested);
			if (!cultureName && requested) {
				// Not a locale Intl knows. Handing the name back unchanged is what keeps
				// custom and legacy culture names usable instead of throwing.
				return giveBackRequestedName();
			}
			const localeName = glue.normalizeLocale(glue.utf16ToString(locale, locale + 2 * localeLength));
			if (!cultureName || !localeName) {
				throw new Error(`Locale or culture name is null or empty. culture=${cultureName}, locale=${localeName}`);
			}

			const parts = cultureName.split('-');
			let languageName;
			let regionName;
			try {
				const region = parts.length > 1 ? parts.pop() : undefined;
				regionName = region ? new Intl.DisplayNames([localeName], { type: 'region' }).of(region) : undefined;
				languageName = new Intl.DisplayNames([localeName], { type: 'language' }).of(parts.join('-'));
			} catch (err) {
				if (!(err instanceof RangeError)) {
					throw err;
				}
				// The split guessed wrong about where the region ends; the whole name may
				// still be a language Intl knows.
				try {
					languageName = new Intl.DisplayNames([localeName], { type: 'language' }).of(cultureName);
				} catch (innerErr) {
					if (innerErr instanceof RangeError && requested) {
						return giveBackRequestedName();
					}
					throw innerErr;
				}
			}

			const result = [languageName, regionName].join('##');
			if (!result) {
				throw new Error(`Locale info for ${cultureName} is null or empty.`);
			}
			if (result.length > dstMaxLength) {
				throw new Error(`Locale info for ${cultureName} exceeds length of ${dstMaxLength}.`);
			}
			glue.stringToUTF16(dst, dst + 2 * result.length, result);
			HEAP32[dstLength >> 2] = result.length;
			return 0;
		} catch (err) {
			HEAP32[dstLength >> 2] = -1;
			return glue.stringToUTF16Ptr(glue.describeError(err));
		}
	},
};

mergeInto(LibraryManager.library, GodotMono);
