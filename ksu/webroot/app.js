/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * KernelSU module WebUI for droid-lkm-suite.
 *
 * The bridge contract is the one KernelSU documents for `kernelsu.spawn` /
 * `kernelsu.exec`:  ksu.exec(command, jsonOptions, callbackName) and the
 * manager then calls window[callbackName](errno, stdout, stderr).  It is NOT a
 * Promise API.  Calling it as if it returned a Promise resolves to undefined
 * immediately, which is exactly the "every command shows no output" bug this
 * rewrite fixes.
 *
 * Nothing here fabricates a result: without the bridge every action is
 * disabled, and output always shows the real errno/stdout/stderr.
 */
(function () {
	"use strict";

	var MODDIR = "/data/adb/modules/droid_lkm_suite";
	var TOOL = MODDIR + "/bin/dnskctl";
	var LANG_KEY = "dnsk.webui.lang";
	var POLL_MS = 4000;
	var MODULES = ["core", "ns", "devtmpfs", "netfilter", "ipset", "cgpids", "tmpfsx"];

	var I18N = {
		zh: {
			title: "Droid LKM Suite",
			kmiPending: "读取内核信息…",
			bridgeChecking: "正在检测 KSU WebUI 桥…",
			bridgeOk: "KSU WebUI 桥可用，命令通过 dnskctl 以 root 执行",
			bridgeMissing: "KSU WebUI 桥不可用（缺少 window.ksu.exec）。所有操作已禁用，本页不会显示任何虚构结果。",
			kpiHeading: "状态概览",
			kpiLoaded: "已加载",
			kpiTotal: "模块总数",
			kpiUnloadable: "可安全卸载",
			actionsTitle: "全部模块",
			startAll: "一键启动全部",
			stopAll: "全部卸载",
			refresh: "刷新",
			modulesTitle: "模块状态",
			diagTitle: "诊断",
			emptyModules: "还没有读取到模块状态。",
			outputTitle: "输出",
			close: "关闭",
			btnCap: "能力",
			btnProbe: "探测",
			btnDoctor: "Doctor",
			btnLogs: "日志",
			absent: "未加载",
			unloadable: "可卸载",
			locked: "需重启",
			load: "加载",
			unload: "卸载",
			refcnt: "引用",
			holders: "被引用",
			running: "执行中…",
			done: "退出码",
			monitorFailed: "状态读取失败",
			busy: "上一个命令尚未结束。",
			unloadConfirm: "卸载会先还原内核 hook，再移除模块。继续？",
			stopAllConfirm: "将按依赖逆序卸载全部模块，并逐项校验内核无告警。继续？",
			noBridge: "命令未执行：KSU WebUI 桥不可用。",
			cancel: "取消",
			confirmAction: "继续",
			exitUnknown: "未知"
		},
		en: {
			title: "Droid LKM Suite",
			kmiPending: "Reading kernel info…",
			bridgeChecking: "Checking the KSU WebUI bridge…",
			bridgeOk: "KSU WebUI bridge available; commands run through dnskctl as root",
			bridgeMissing: "KSU WebUI bridge unavailable (window.ksu.exec is missing). Every action is disabled and this page shows no fabricated result.",
			kpiHeading: "Overview",
			kpiLoaded: "Loaded",
			kpiTotal: "Modules",
			kpiUnloadable: "Safely unloadable",
			actionsTitle: "All modules",
			startAll: "Start all",
			stopAll: "Stop all",
			refresh: "Refresh",
			modulesTitle: "Module status",
			diagTitle: "Diagnostics",
			emptyModules: "No module state has been read yet.",
			outputTitle: "Output",
			close: "Close",
			btnCap: "Capabilities",
			btnProbe: "Probe",
			btnDoctor: "Doctor",
			btnLogs: "Logs",
			absent: "Not loaded",
			unloadable: "Unloadable",
			locked: "Needs reboot",
			load: "Load",
			unload: "Unload",
			refcnt: "refs",
			holders: "held by",
			running: "running…",
			done: "exit",
			monitorFailed: "Could not read state",
			busy: "The previous command is still running.",
			unloadConfirm: "Unloading restores kernel hooks first, then removes the module. Continue?",
			stopAllConfirm: "This unloads every module in reverse dependency order and checks the kernel for alarms. Continue?",
			noBridge: "Command not executed: the KSU WebUI bridge is unavailable.",
			cancel: "Cancel",
			confirmAction: "Continue",
			exitUnknown: "unknown"
		}
	};

	var lang = "en";
	var busy = false;
	var runStarted = false;
	var pollTimer = null;
	var lastState = null;

	var el = {
		out: document.getElementById("out"),
		outMeta: document.getElementById("out-meta"),
		bridge: document.getElementById("bridge"),
		list: document.getElementById("module-list"),
		empty: document.getElementById("module-empty"),
		kmi: document.getElementById("kmi-line"),
		kpiLoaded: document.getElementById("kpi-loaded"),
		kpiTotal: document.getElementById("kpi-total"),
		kpiUnloadable: document.getElementById("kpi-unloadable"),
		sheet: document.getElementById("sheet"),
		backdrop: document.getElementById("sheet-backdrop"),
		close: document.getElementById("sheet-close"),
		startAll: document.getElementById("start-all"),
		stopAll: document.getElementById("stop-all"),
		refresh: document.getElementById("refresh"),
		confirm: document.getElementById("confirm"),
		confirmBackdrop: document.getElementById("confirm-backdrop"),
		confirmText: document.getElementById("confirm-text"),
		confirmOk: document.getElementById("confirm-ok"),
		confirmCancel: document.getElementById("confirm-cancel")
	};

	function t(key) {
		var table = I18N[lang] || I18N.en;
		return Object.prototype.hasOwnProperty.call(table, key) ? table[key] : key;
	}

	/* ---------------- KSU bridge ---------------- */

	function bridge() {
		if (typeof window.ksu === "undefined" || window.ksu === null) {
			return null;
		}
		if (typeof window.ksu.exec !== "function") {
			return null;
		}
		return window.ksu;
	}

	var callbackSeq = 0;

	/*
	 * exec runs `command` through the manager and resolves with the real
	 * {errno, stdout, stderr}.  The manager reports results by invoking a
	 * global function whose name we hand it, so the promise is settled from
	 * that callback (or rejected on a timeout if the bridge never answers).
	 */
	function exec(command, options) {
		return new Promise(function (resolve, reject) {
			var api = bridge();
			if (!api) {
				reject(new Error(t("noBridge")));
				return;
			}
			callbackSeq += 1;
			var name = "__dnsk_exec_" + Date.now() + "_" + callbackSeq;
			var settled = false;

			function cleanup() {
				try {
					delete window[name];
				} catch (err) {
					window[name] = undefined;
				}
			}

			window[name] = function (errno, stdout, stderr) {
				if (settled) {
					return;
				}
				settled = true;
				cleanup();
				resolve({
					errno: typeof errno === "number" ? errno : Number(errno),
					stdout: typeof stdout === "string" ? stdout : (stdout == null ? "" : String(stdout)),
					stderr: typeof stderr === "string" ? stderr : (stderr == null ? "" : String(stderr))
				});
			};

			try {
				api.exec(command, JSON.stringify(options || {}), name);
			} catch (err) {
				settled = true;
				cleanup();
				reject(err);
				return;
			}

			/* A silent bridge must not leave the UI stuck on "running". */
			setTimeout(function () {
				if (settled) {
					return;
				}
				settled = true;
				cleanup();
				reject(new Error(t("monitorFailed") + " (bridge timeout)"));
			}, 60000);
		});
	}

	function hasBridge() {
		return bridge() !== null;
	}

	function setBridgeState(available) {
		if (available) {
			el.bridge.className = "banner banner-ok";
			el.bridge.textContent = t("bridgeOk");
		} else {
			el.bridge.className = "banner banner-warn";
			el.bridge.textContent = t("bridgeMissing");
		}
		syncControls();
	}

	function syncControls() {
		var ok = hasBridge() && !busy;
		[el.startAll, el.stopAll, el.refresh].forEach(function (node) {
			if (node) {
				node.disabled = !ok;
			}
		});
		Array.prototype.forEach.call(
			document.querySelectorAll("[data-cmd]"),
			function (node) {
				node.disabled = !ok;
			}
		);
		renderList(lastState);
	}

	/* ---------------- in-page confirm ----------------
	 *
	 * window.confirm() is unreliable inside an Android WebView: unless the host
	 * activity implements onJsConfirm the dialog never appears and the call
	 * returns false, which would make the button look dead.  Both destructive
	 * actions therefore ask through the page itself.
	 */

	var confirmState = null;

	function askConfirm(message) {
		return new Promise(function (resolve) {
			confirmState = resolve;
			el.confirmText.textContent = message;
			el.confirm.hidden = false;
			el.confirmBackdrop.hidden = false;
			el.confirmOk.focus();
		});
	}

	function settleConfirm(value) {
		if (!confirmState) {
			return;
		}
		var resolve = confirmState;
		confirmState = null;
		el.confirm.hidden = true;
		el.confirmBackdrop.hidden = true;
		resolve(value);
	}

	function openSheet() {
		el.sheet.hidden = false;
		el.backdrop.hidden = false;
		document.body.classList.add("sheet-open");
	}

	function closeSheet() {
		el.sheet.hidden = true;
		el.backdrop.hidden = true;
		document.body.classList.remove("sheet-open");
	}

	function writeOutput(header, text) {
		el.outMeta.textContent = header;
		el.out.textContent = text;
		el.out.scrollTop = 0;
	}

	function showCommandResult(command, res) {
		var code = res && typeof res.errno === "number" && isFinite(res.errno) ?
			String(res.errno) : t("exitUnknown");
		var parts = [];
		if (res && res.stdout) {
			parts.push(res.stdout.replace(/\s+$/, ""));
		}
		if (res && res.stderr) {
			parts.push("[stderr]\n" + res.stderr.replace(/\s+$/, ""));
		}
		writeOutput("$ " + command + "\n" + t("done") + ": " + code,
			parts.length ? parts.join("\n") : "—");
		openSheet();
	}

	function run(command, options) {
		if (busy) {
			writeOutput("$ " + command, t("busy"));
			openSheet();
			return Promise.resolve(null);
		}
		if (!hasBridge()) {
			writeOutput("$ " + command, t("noBridge"));
			openSheet();
			setBridgeState(false);
			return Promise.resolve(null);
		}
		busy = true;
		runStarted = true;
		writeOutput("$ " + command, t("running"));
		openSheet();
		syncControls();

		return exec(command, options).then(function (res) {
			showCommandResult(command, res);
			return res;
		}, function (err) {
			var message = err && err.message ? err.message : String(err);
			writeOutput("$ " + command, message);
			openSheet();
			return null;
		}).then(function (res) {
			busy = false;
			syncControls();
			return res;
		});
	}

	/* ---------------- state polling ---------------- */

	function parseMonitor(res) {
		if (!res || typeof res.stdout !== "string" || res.stdout === "") {
			return null;
		}
		var line = res.stdout.trim().split("\n").pop();
		try {
			return JSON.parse(line);
		} catch (err) {
			return null;
		}
	}

	function moduleState(state, name) {
		if (!state || !state.modules || !state.modules[name]) {
			return null;
		}
		return state.modules[name];
	}

	/* A module can be dropped by rmmod only when nothing else references it
	 * and no live namespace keeps calling into it. */
	function isUnloadable(entry) {
		if (!entry || entry.state !== "loaded") {
			return false;
		}
		var refs = String(entry.refcnt);
		if (refs === "-" || refs === "") {
			return false;
		}
		if (Number(refs) !== 0) {
			return false;
		}
		return !entry.holders;
	}

	function renderList(state) {
		if (!el.list) {
			return;
		}
		while (el.list.firstChild) {
			el.list.removeChild(el.list.firstChild);
		}
		var any = false;
		MODULES.forEach(function (name) {
			var entry = moduleState(state, name);
			if (!entry) {
				return;
			}
			any = true;
			el.list.appendChild(buildRow(name, entry));
		});
		if (el.empty) {
			el.empty.hidden = any;
		}
	}

	function buildRow(name, entry) {
		var li = document.createElement("li");
		li.className = "module-item";

		var main = document.createElement("div");
		main.className = "module-main";

		var title = document.createElement("div");
		title.className = "module-name";
		var code = document.createElement("code");
		code.textContent = name;
		title.appendChild(code);
		title.appendChild(badgeFor(entry));
		main.appendChild(title);

		var meta = document.createElement("p");
		meta.className = "module-meta";
		meta.textContent = metaText(entry);
		main.appendChild(meta);

		li.appendChild(main);

		var action = document.createElement("button");
		action.type = "button";
		action.className = "module-action";
		var loaded = entry.state === "loaded";
		action.textContent = loaded ? t("unload") : t("load");
		/* A pinned module is still offered: dnskctl refuses with the real
		 * reason instead of the UI silently hiding the action. */
		action.disabled = busy || !hasBridge();
		action.addEventListener("click", function () {
			if (!loaded) {
				run(TOOL + " load " + name, { cwd: MODDIR }).then(refreshState);
				return;
			}
			askConfirm(t("unloadConfirm")).then(function (ok) {
				if (ok) {
					run(TOOL + " unload " + name, { cwd: MODDIR }).then(refreshState);
				}
			});
		});
		li.appendChild(action);
		return li;
	}

	function badgeFor(entry) {
		var span = document.createElement("span");
		if (entry.state !== "loaded") {
			span.className = "badge badge-idle";
			span.textContent = t("absent");
			return span;
		}
		if (isUnloadable(entry)) {
			span.className = "badge badge-ok";
			span.textContent = t("unloadable");
			return span;
		}
		span.className = "badge badge-lock";
		span.textContent = t("locked");
		return span;
	}

	function metaText(entry) {
		var bits = [];
		if (entry.refcnt !== undefined && entry.refcnt !== null) {
			bits.push(t("refcnt") + " " + entry.refcnt);
		}
		if (entry.holders) {
			bits.push(t("holders") + " " + entry.holders.replace(/\s+$/, ""));
		}
		return bits.join(" · ");
	}

	function refreshState() {
		if (!hasBridge()) {
			return Promise.resolve(null);
		}
		return exec(TOOL + " monitor --json", { cwd: MODDIR }).then(function (res) {
			var state = parseMonitor(res);
			if (!state) {
				return null;
			}
			lastState = state;
			applyState(state);
			return state;
		}, function () {
			return null;
		});
	}

	function applyState(state) {
		var total = 0;
		var loaded = 0;
		var unloadable = 0;
		MODULES.forEach(function (name) {
			var entry = moduleState(state, name);
			if (!entry) {
				return;
			}
			total += 1;
			if (entry.state === "loaded") {
				loaded += 1;
				if (isUnloadable(entry)) {
					unloadable += 1;
				}
			}
		});
		el.kpiLoaded.textContent = String(loaded);
		el.kpiTotal.textContent = String(total);
		el.kpiUnloadable.textContent = String(unloadable);
		el.kmi.textContent = (state.kmi || "—") +
			(state.core_loaded ? " · core" : "");
		renderList(state);
	}

	function schedulePoll() {
		if (pollTimer) {
			window.clearTimeout(pollTimer);
		}
		pollTimer = window.setTimeout(function () {
			refreshState().then(schedulePoll, schedulePoll);
		}, POLL_MS);
	}

	/* ---------------- language ---------------- */

	function applyLang(next) {
		lang = next === "zh" ? "zh" : "en";
		document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
		Array.prototype.forEach.call(
			document.querySelectorAll("[data-i18n]"),
			function (node) {
				var key = node.getAttribute("data-i18n");
				if (key) {
					node.textContent = t(key);
				}
			}
		);
		Array.prototype.forEach.call(
			document.querySelectorAll("[data-i18n-label]"),
			function (node) {
				var key = node.getAttribute("data-i18n-label");
				if (key) {
					node.setAttribute("aria-label", t(key));
				}
			}
		);
		Array.prototype.forEach.call(
			document.querySelectorAll("[data-lang]"),
			function (btn) {
				btn.setAttribute("aria-pressed",
					btn.getAttribute("data-lang") === lang ? "true" : "false");
			}
		);
		setBridgeState(hasBridge());
		if (lastState) {
			applyState(lastState);
		}
		if (!runStarted) {
			writeOutput(t("outputTitle"), "—");
		}
		try {
			window.localStorage.setItem(LANG_KEY, lang);
		} catch (err) {
			/* storage may be unavailable inside the WebView sandbox */
		}
	}

	/* ---------------- wiring ---------------- */

	Array.prototype.forEach.call(
		document.querySelectorAll("[data-lang]"),
		function (btn) {
			btn.addEventListener("click", function () {
				applyLang(btn.getAttribute("data-lang"));
			});
		}
	);

	Array.prototype.forEach.call(
		document.querySelectorAll("[data-cmd]"),
		function (btn) {
			btn.addEventListener("click", function () {
				run(TOOL + " " + btn.getAttribute("data-cmd"), { cwd: MODDIR });
			});
		}
	);

	if (el.startAll) {
		el.startAll.addEventListener("click", function () {
			run(TOOL + " start-all", { cwd: MODDIR })
				.then(refreshState);
		});
	}

	if (el.stopAll) {
		el.stopAll.addEventListener("click", function () {
			askConfirm(t("stopAllConfirm")).then(function (ok) {
				if (ok) {
					run(TOOL + " stop-all", { cwd: MODDIR })
						.then(refreshState);
				}
			});
		});
	}

	if (el.refresh) {
		el.refresh.addEventListener("click", function () {
			refreshState();
		});
	}

	if (el.close) {
		el.close.addEventListener("click", closeSheet);
	}
	if (el.backdrop) {
		el.backdrop.addEventListener("click", closeSheet);
	}
	if (el.confirmOk) {
		el.confirmOk.addEventListener("click", function () {
			settleConfirm(true);
		});
	}
	if (el.confirmCancel) {
		el.confirmCancel.addEventListener("click", function () {
			settleConfirm(false);
		});
	}
	/* A stray tap outside the dialog cancels instead of silently proceeding:
	 * destructive actions must never start from an accidental touch. */
	if (el.confirmBackdrop) {
		el.confirmBackdrop.addEventListener("click", function () {
			settleConfirm(false);
		});
	}

	document.addEventListener("visibilitychange", function () {
		if (document.hidden) {
			if (pollTimer) {
				window.clearTimeout(pollTimer);
				pollTimer = null;
			}
			return;
		}
		refreshState().then(schedulePoll, schedulePoll);
	});

	var saved = null;
	try {
		saved = window.localStorage.getItem(LANG_KEY);
	} catch (err) {
		saved = null;
	}
	if (saved === "zh" || saved === "en") {
		lang = saved;
	} else {
		lang = (navigator.language || "").toLowerCase().indexOf("zh") === 0 ?
			"zh" : "en";
	}
	applyLang(lang);
	closeSheet();
	refreshState().then(schedulePoll, schedulePoll);
})();
