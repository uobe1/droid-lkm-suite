#!/usr/bin/env node
/*
 * Offline behavioural test for the module WebUI.
 *
 * The point of this test is the bug it guards: the page used to call
 * `window.ksu.exec(cmd, opts)` and treat the return value as a Promise.  The
 * real bridge takes a callback *name* and later invokes
 * window[<name>](errno, stdout, stderr); nothing is returned.  So the old code
 * resolved to undefined and every command appeared to produce no output.
 *
 * Runs without Chrome: it loads app.js into a minimal DOM shim and a fake KSU
 * bridge that follows the documented contract.
 *
 * usage: node tests/host/webui-bridge.test.js
 */
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const WEBROOT = path.join(__dirname, "..", "..", "ksu", "webroot");
const APP = path.join(WEBROOT, "app.js");

let failures = 0;
const results = [];

/*
 * Tests may be synchronous or async.  An async test that rejects must fail the
 * run: calling fn() and ignoring the returned promise turns every async
 * assertion into a no-op, which is exactly how a suite ends up green while the
 * behaviour it claims to test is broken.
 */
async function check(name, fn) {
	try {
		await fn();
		results.push({ name, ok: true });
	} catch (err) {
		failures += 1;
		results.push({ name, ok: false, detail: err && err.message ? err.message : String(err) });
	}
}

function assert(cond, message) {
	if (!cond) throw new Error(message);
}

/* ---------------- minimal DOM ---------------- */

function makeClassList() {
	const set = new Set();
	return {
		add: (c) => set.add(c),
		remove: (c) => set.delete(c),
		contains: (c) => set.has(c),
	};
}

function makeElement(tagName) {
	const node = {
		tagName: String(tagName || "div").toUpperCase(),
		children: [],
		attributes: {},
		style: {},
		dataset: {},
		className: "",
		textContent: "",
		disabled: false,
		hidden: false,
		value: "",
		scrollTop: 0,
		classList: makeClassList(),
		appendChild(child) {
			this.children.push(child);
			child.parentNode = this;
			return child;
		},
		removeChild(child) {
			const i = this.children.indexOf(child);
			if (i >= 0) this.children.splice(i, 1);
			return child;
		},
		setAttribute(name, value) {
			this.attributes[name] = String(value);
			if (name === "data-lang") this.dataset.lang = String(value);
			if (name === "data-cmd") this.dataset.cmd = String(value);
		},
		getAttribute(name) {
			return Object.prototype.hasOwnProperty.call(this.attributes, name)
				? this.attributes[name]
				: null;
		},
		addEventListener(type, handler) {
			this.listeners = this.listeners || {};
			(this.listeners[type] = this.listeners[type] || []).push(handler);
		},
		click() {
			const list = (this.listeners && this.listeners.click) || [];
			list.forEach((fn) => fn());
		},
		focus() {
			this.focused = true;
		},
		get firstChild() {
			return this.children.length ? this.children[0] : null;
		},
	};
	return node;
}

function buildDom(html) {
	const byId = Object.create(null);

	/*
	 * Collect id="..." occurrences and pre-create the elements app.js asks for.
	 * The element's initial visibility must come from the markup: several
	 * elements start hidden (the output sheet and the confirmation dialog), and
	 * a shim that defaults everything to visible makes "is it shown?" assertions
	 * pass when nothing was ever opened.
	 */
	const tagRe = /<(\w+)([^>]*)>/g;
	let m;
	while ((m = tagRe.exec(html)) !== null) {
		const attrs = m[2];
		const idMatch = /id="([^"]+)"/.exec(attrs);
		if (!idMatch) {
			continue;
		}
		const node = makeElement(m[1]);
		node.hidden = /\bhidden\b/.test(attrs);
		byId[idMatch[1]] = node;
	}

	const langButtons = ["zh", "en"].map((code) => {
		const b = makeElement("button");
		b.setAttribute("data-lang", code);
		return b;
	});
	const cmdButtons = ["cap", "probe", "doctor --last-panic", "logs 100"].map((cmd) => {
		const b = makeElement("button");
		b.setAttribute("data-cmd", cmd);
		return b;
	});

	const document = {
		documentElement: { lang: "en" },
		hidden: false,
		body: makeElement("body"),
		getElementById(id) {
			return byId[id] || null;
		},
		createElement(tag) {
			return makeElement(tag);
		},
		querySelectorAll(selector) {
			if (selector === "[data-lang]") return langButtons;
			if (selector === "[data-cmd]") return cmdButtons;
			if (selector.indexOf("[data-i18n") === 0) return [];
			return [];
		},
		addEventListener() {},
	};

	return { document, byId, langButtons, cmdButtons };
}

/* ---------------- fake KernelSU bridge ---------------- */

function makeBridge(behaviour) {
	let callCount = 0;
	const calls = [];
	const bridge = {
		calls,
		get callCount() {
			return callCount;
		},
		exec(command, optionsJson, callbackName) {
			callCount += 1;
			calls.push({ command, optionsJson, callbackName });
			let options = {};
			try {
				options = JSON.parse(optionsJson);
			} catch (err) {
				throw new Error("bridge received invalid options JSON: " + optionsJson);
			}
			if (typeof callbackName !== "string" || callbackName === "") {
				throw new Error("bridge requires a callback name, got " + typeof callbackName);
			}
			const outcome = behaviour(command, options, callCount);
			/* The real manager answers asynchronously through the global. */
			setTimeout(() => {
				const cb = bridge.scope ? bridge.scope[callbackName] : undefined;
				if (typeof cb !== "function") {
					behaviour.missingCallback && behaviour.missingCallback(command, callbackName);
					return;
				}
				cb(outcome.errno, outcome.stdout, outcome.stderr);
			}, 0);
			/* Deliberately return undefined: the bridge is callback based. */
			return undefined;
		},
	};
	return bridge;
}

function makeContext(dom, bridge, storage) {
	const context = {
		document: dom.document,
		navigator: { language: "en-US" },
		window: null,
		console,
		setTimeout,
		clearTimeout,
		Date,
		Promise,
		JSON,
		Number,
		String,
		Object,
		Array,
		isFinite,
	};
	context.window = context;
	context.localStorage = storage || {
		getItem: () => null,
		setItem: () => {},
	};
	if (bridge) {
		context.ksu = bridge;
		bridge.scope = context;
	}
	context.confirm = () => true;
	return context;
}

function loadApp(dom, bridge, storage) {
	const script = fs.readFileSync(APP, "utf8");
	const context = makeContext(dom, bridge, storage);
	vm.createContext(context);
	vm.runInContext(script, context, { filename: "app.js" });
	return context;
}

/* Wait for queued bridge callbacks and promise chains to settle. */
function settle(ms) {
	return new Promise((resolve) => setTimeout(resolve, ms));
}

/* ---------------- the bug the rewrite fixes ---------------- */

function oldStyleResolvesToUndefined(bridge, command) {
	/* Exactly what the previous implementation did: treat the bridge as a
	 * Promise-returning API and never register a callback. */
	let pending;
	let rejected = false;
	try {
		pending = Promise.resolve(bridge.exec(command, JSON.stringify({ cwd: "/x" })));
	} catch (err) {
		rejected = true;
		pending = Promise.resolve(undefined);
	}
	let value = "unset";
	pending.then(
		(res) => {
			value = res;
		},
		() => {
			rejected = true;
			value = undefined;
		}
	);
	return { pending, read: () => value, get rejected() { return rejected; } };
}

async function main() {
	const html = fs.readFileSync(path.join(WEBROOT, "index.html"), "utf8");

	await check("index.html is self-contained (no external resources)", () => {
		assert(!/<script[^>]+src="https?:/i.test(html), "external script found");
		assert(!/<link[^>]+href="https?:/i.test(html), "external stylesheet found");
		assert(/viewport-fit=cover/.test(html), "viewport-fit=cover missing");
	});

	await check("exactly one primary button in the action area", () => {
		const primary = (html.match(/btn-primary/g) || []).length;
		assert(primary === 1, "expected 1 btn-primary, found " + primary);
	});

	await check("old Promise-style bridge call yields no output (regression proof)", async () => {
		/* A bridge that only answers through the documented callback.  There is
		 * nothing to resolve, so the old code could never see stdout. */
		const bridge = makeBridge(() => ({ errno: 0, stdout: "hello\n", stderr: "" }));
		const probe = oldStyleResolvesToUndefined(bridge, "/data/adb/modules/x/bin/dnskctl status");
		await settle(10);
		/* Either the call was rejected (no callback name) or it resolved to
		 * undefined: in both cases the old page rendered nothing. */
		assert(probe.rejected || probe.read() === undefined,
			"old style must produce no usable result");
		assert(probe.read() !== "hello\n", "old style must never see stdout");
	});

	let captured = null;

	await check("new bridge call passes a callback name and receives real stdout", async () => {
		const bridge = makeBridge((command) => {
			if (command.indexOf("monitor --json") >= 0) {
				return { errno: 0, stdout: JSON.stringify({
					kmi: "android16-6.12",
					core_loaded: true,
					modules: {
						core: { state: "loaded", refcnt: "0", holders: "" },
						ns: { state: "loaded", refcnt: "0", holders: "" },
						devtmpfs: { state: "loaded", refcnt: "0", holders: "" },
						netfilter: { state: "absent", refcnt: "-", holders: "" },
						ipset: { state: "loaded", refcnt: "0", holders: "" },
						cgpids: { state: "loaded", refcnt: "0", holders: "" },
						tmpfsx: { state: "loaded", refcnt: "0", holders: "" },
					},
				}) + "\n", stderr: "" };
			}
			return { errno: 0, stdout: "real output\n", stderr: "" };
		});
		const dom = buildDom(html);
		const ctx = loadApp(dom, bridge, null);
		await settle(20);

		/* The page must have polled monitor on load. */
		assert(bridge.callCount >= 1, "expected an initial monitor poll");
		const first = bridge.calls[0];
		assert(/monitor --json/.test(first.command), "first call should be monitor, got " + first.command);
		assert(typeof first.callbackName === "string" && first.callbackName !== "",
			"exec must be given a callback name");

		/* Now press a diagnostic button and verify the output panel updates. */
		const probeButton = dom.cmdButtons[1];
		probeButton.click();
		await settle(30);
		captured = {
			out: ctx.document.getElementById("out").textContent,
			meta: ctx.document.getElementById("out-meta").textContent,
		};
		assert(/real output/.test(captured.out),
			"stdout did not reach the output panel: " + JSON.stringify(captured.out));
		assert(/probe/.test(captured.meta), "output header should name the command");
	});

	await check("exit code is surfaced, not hidden", async () => {
		const bridge = makeBridge((command) => {
			if (command.indexOf("monitor --json") >= 0) {
				return { errno: 0, stdout: JSON.stringify({ modules: {} }), stderr: "" };
			}
			return { errno: 3, stdout: "", stderr: "boom\n" };
		});
		const dom = buildDom(html);
		const ctx = loadApp(dom, bridge, null);
		await settle(20);
		dom.cmdButtons[0].click();
		await settle(30);
		const meta = ctx.document.getElementById("out-meta").textContent;
		const out = ctx.document.getElementById("out").textContent;
		assert(/3/.test(meta), "exit code 3 missing from header: " + meta);
		assert(/boom/.test(out), "stderr missing from output: " + out);
	});

	await check("module list renders state and badges from monitor JSON", async () => {
		const bridge = makeBridge((command) => {
			if (command.indexOf("monitor --json") >= 0) {
				return { errno: 0, stdout: JSON.stringify({
					kmi: "android16-6.12",
					core_loaded: true,
					modules: {
						core: { state: "loaded", refcnt: "5", holders: "dnsk_ns" },
						ns: { state: "loaded", refcnt: "0", holders: "" },
						ipset: { state: "absent", refcnt: "-", holders: "" },
					},
				}), stderr: "" };
			}
			return { errno: 0, stdout: "", stderr: "" };
		});
		const dom = buildDom(html);
		const ctx = loadApp(dom, bridge, null);
		await settle(20);
		const list = ctx.document.getElementById("module-list");
		assert(list.children.length === 3, "expected 3 rows, got " + list.children.length);
		const kpis = [
			ctx.document.getElementById("kpi-loaded").textContent,
			ctx.document.getElementById("kpi-total").textContent,
			ctx.document.getElementById("kpi-unloadable").textContent,
		];
		assert(kpis[0] === "2", "loaded KPI should be 2, got " + kpis[0]);
		assert(kpis[1] === "3", "total KPI should be 3, got " + kpis[1]);
		assert(kpis[2] === "1", "unloadable KPI should be 1, got " + kpis[2]);
	});

	await check("missing bridge disables every action and says so", async () => {
		const dom = buildDom(html);
		const ctx = loadApp(dom, null, null);
		await settle(20);
		const banner = ctx.document.getElementById("bridge");
		assert(/unavailable|不可用/i.test(banner.textContent),
			"bridge banner should report the missing bridge: " + banner.textContent);
		const startAll = ctx.document.getElementById("start-all");
		assert(startAll.disabled === true, "start-all must be disabled without the bridge");
	});

	await check("start-all and stop-all map to the documented dnskctl commands", async () => {
		const seen = [];
		const bridge = makeBridge((command) => {
			seen.push(command);
			if (command.indexOf("monitor --json") >= 0) {
				return { errno: 0, stdout: JSON.stringify({ modules: {} }), stderr: "" };
			}
			return { errno: 0, stdout: "ok\n", stderr: "" };
		});
		const dom = buildDom(html);
		const ctx = loadApp(dom, bridge, null);
		await settle(20);
		ctx.document.getElementById("start-all").click();
		/* Wait for the run to finish: the page refuses a second command while
		 * one is in flight, so clicking stop-all too early would test the busy
		 * guard instead of the confirmation flow. */
		for (let i = 0; i < 20 && !seen.some((c) => /dnskctl start-all$/.test(c)); i += 1) {
			await settle(10);
		}
		assert(seen.some((c) => /dnskctl start-all$/.test(c)),
			"start-all command not issued: " + JSON.stringify(seen));
		await settle(30);

		/* stop-all is destructive: it must ask first and stay silent until the
		 * user confirms.  A test that only checks "the command ran" would pass
		 * even if the confirmation was skipped entirely. */
		const before = seen.length;
		ctx.document.getElementById("stop-all").click();
		await settle(30);
		assert(!seen.some((c) => /dnskctl stop-all$/.test(c)),
			"stop-all must not run before confirmation: " + JSON.stringify(seen));
		assert(ctx.document.getElementById("confirm").hidden === false,
			"confirmation dialog should be visible");

		ctx.document.getElementById("confirm-ok").click();
		await settle(30);
		assert(seen.some((c) => /dnskctl stop-all$/.test(c)),
			"stop-all should run after confirmation: " + JSON.stringify(seen));
		assert(ctx.document.getElementById("confirm").hidden === true,
			"confirmation dialog should close once answered");
	});

	/* Report */
	const pad = results.reduce((n, r) => Math.max(n, r.name.length), 0);
	results.forEach((r) => {
		const label = r.name.padEnd(pad);
		if (r.ok) {
			console.log("ok   " + label);
		} else {
			console.log("FAIL " + label + "  " + r.detail);
		}
	});
	console.log("");
	console.log(failures === 0 ?
		"WEBUI_BRIDGE_OK " + results.length + "/" + results.length :
		"WEBUI_BRIDGE_FAIL " + failures + "/" + results.length);
	process.exit(failures === 0 ? 0 : 1);
}

main().catch((err) => {
	console.error("harness error:", err && err.stack ? err.stack : err);
	process.exit(2);
});
