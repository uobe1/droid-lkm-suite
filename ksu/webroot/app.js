/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Local-only WebUI for KernelSU.  It shells out to dnskctl through the KSU
 * bridge and renders the real stdout/stderr/exit code.  It never fabricates a
 * result: without window.ksu.exec every command is disabled and the page says
 * so.  No innerHTML is used anywhere; all command output goes to textContent.
 */
(function () {
	"use strict";

	var MODDIR = "/data/adb/modules/droid_lkm_suite";
	var TOOL = MODDIR + "/bin/dnskctl";
	var LANG_KEY = "dnsk.webui.lang";

	var I18N = {
		zh: {
			title: "Droid LKM Suite",
			subtitle: "android16-6.12 开发构建 · 以下结果全部来自 dnskctl 的真实输出",
			bridgeChecking: "正在检测 KSU WebUI 桥…",
			bridgeOk: "KSU WebUI 桥可用：命令将通过 dnskctl 以 root 执行",
			bridgeMissing: "KSU WebUI 桥不可用（缺少 window.ksu.exec）：无法执行任何命令，本页面也不会显示任何虚构的成功结果。",
			cmdTitle: "命令",
			btnStatus: "状态",
			btnCap: "能力",
			btnProbe: "探测",
			btnDoctor: "Doctor（上次 panic）",
			btnLogs: "日志（100 行）",
			btnBoot: "启动检查",
			btnLoad: "加载",
			btnUnload: "卸载",
			btnProfile: "执行 profile",
			featureLabel: "feature 模块",
			profileLabel: "profile",
			capTitle: "能力等级（静态声明）",
			thFeature: "功能",
			thLevel: "声明等级",
			thNotes: "说明",
			capNote: "以上等级取自 docs/status.md（R = 原生语义替代，D = 设备差分，V = 虚拟控制器，N = 原生），只是声明，不代表当前已加载；实际状态请以 status 的真实输出为准。本页不会显示没有证据支撑的“成功”。",
			outTitle: "输出",
			clear: "清空",
			prompt: "点击上方按钮执行命令。",
			running: "执行中…",
			exitCode: "退出码",
			stdoutLabel: "stdout",
			stderrLabel: "stderr",
			empty: "（空）",
			unknown: "未知",
			execFailed: "调用 dnskctl 失败",
			bridgeDenied: "命令未执行：KSU WebUI 桥不可用。",
			busy: "上一个命令尚未结束。",
			capNs: "PID/IPC 命名空间、SYSVIPC、POSIX MQUEUE（Droidspaces 移植，含嵌套）",
			capIpset: "IP_SET、hash:ip、hash:net、xt_set",
			capNetfilter: "xt addrtype、nft 子集、REJECT、RECENT、IPv6 NAT",
			capTmpfsx: "tmpfs user.* xattr 与 POSIX ACL（全局 Native 语义）",
			capCgpids: "虚拟 pids 控制器（仅 cgroup v2）",
			capUserns: "USER_NS 模拟、idmapped/子 userns overlay、FUSE"
		},
		en: {
			title: "Droid LKM Suite",
			subtitle: "android16-6.12 dev build. Every result below is real dnskctl output.",
			bridgeChecking: "Checking the KSU WebUI bridge…",
			bridgeOk: "KSU WebUI bridge available: commands run through dnskctl as root",
			bridgeMissing: "KSU WebUI bridge unavailable (window.ksu.exec is missing): no command can run and this page will not show any fabricated success.",
			cmdTitle: "Commands",
			btnStatus: "Status",
			btnCap: "Capabilities",
			btnProbe: "Probe",
			btnDoctor: "Doctor (last panic)",
			btnLogs: "Logs (100)",
			btnBoot: "Boot check",
			btnLoad: "Load",
			btnUnload: "Unload",
			btnProfile: "Run profile",
			featureLabel: "feature module",
			profileLabel: "profile",
			capTitle: "Capability levels (static declaration)",
			thFeature: "Feature",
			thLevel: "Declared level",
			thNotes: "Notes",
			capNote: "Levels come from docs/status.md (R = native semantic replacement, D = device differential, V = virtual controller, N = native). They are a declaration, not a live load state; trust only what status prints. This page never shows unsupported success.",
			outTitle: "Output",
			clear: "Clear",
			prompt: "Pick a command above.",
			running: "running…",
			exitCode: "exit code",
			stdoutLabel: "stdout",
			stderrLabel: "stderr",
			empty: "(empty)",
			unknown: "unknown",
			execFailed: "dnskctl invocation failed",
			bridgeDenied: "Command not executed: the KSU WebUI bridge is unavailable.",
			busy: "The previous command is still running.",
			capNs: "PID/IPC namespaces, SYSVIPC, POSIX MQUEUE (Droidspaces port, nested)",
			capIpset: "IP_SET, hash:ip, hash:net, xt_set",
			capNetfilter: "xt addrtype, nft subset, REJECT, RECENT, IPv6 NAT",
			capTmpfsx: "tmpfs user.* xattr and POSIX ACL (global native semantics)",
			capCgpids: "virtual pids controller (cgroup v2 only)",
			capUserns: "USER_NS emulation, idmapped/child userns overlay, FUSE"
		}
	};

	var CAPS = [
		{ name: "ns", level: "R", text: "capNs" },
		{ name: "ipset", level: "R", text: "capIpset" },
		{ name: "netfilter", level: "R", text: "capNetfilter" },
		{ name: "tmpfsx", level: "D\u2192N", text: "capTmpfsx" },
		{ name: "cgpids", level: "V/D\u2192N", text: "capCgpids" },
		{ name: "userns", level: "D\u2192N", text: "capUserns" }
	];

	var outEl = document.getElementById("out");
	var bridgeEl = document.getElementById("bridge");
	var capBody = document.getElementById("cap-body");
	var featureEl = document.getElementById("feature");
	var profileEl = document.getElementById("profile");
	var langButtons = document.querySelectorAll("button[data-lang]");
	var cmdButtons = document.querySelectorAll("button[data-cmd]");
	var clearButton = document.getElementById("clear");

	var lang = "en";
	var busy = false;
	var runStarted = false;

	function t(key) {
		var table = I18N[lang] || I18N.en;
		return Object.prototype.hasOwnProperty.call(table, key) ? table[key] : key;
	}

	function hasBridge() {
		return typeof window.ksu !== "undefined" && window.ksu !== null &&
			typeof window.ksu.exec === "function";
	}

	function describeError(err) {
		if (err && typeof err.message === "string" && err.message !== "") {
			return err.message;
		}
		return String(err);
	}

	function writeOut(text) {
		outEl.textContent = text;
		outEl.scrollTop = 0;
	}

	function renderCapTable() {
		if (!capBody) {
			return;
		}
		while (capBody.firstChild) {
			capBody.removeChild(capBody.firstChild);
		}
		CAPS.forEach(function (cap) {
			var tr = document.createElement("tr");
			var name = document.createElement("td");
			var level = document.createElement("td");
			var notes = document.createElement("td");
			name.textContent = cap.name;
			level.className = "level";
			level.textContent = cap.level;
			notes.textContent = t(cap.text);
			tr.appendChild(name);
			tr.appendChild(level);
			tr.appendChild(notes);
			capBody.appendChild(tr);
		});
	}

	function applyLang(next) {
		lang = next === "zh" ? "zh" : "en";
		document.documentElement.lang = lang === "zh" ? "zh-CN" : "en";
		Array.prototype.forEach.call(document.querySelectorAll("[data-i18n]"), function (el) {
			var key = el.getAttribute("data-i18n");
			if (key) {
				el.textContent = t(key);
			}
		});
		Array.prototype.forEach.call(document.querySelectorAll("[data-i18n-label]"), function (el) {
			var key = el.getAttribute("data-i18n-label");
			if (key) {
				el.setAttribute("aria-label", t(key));
			}
		});
		Array.prototype.forEach.call(langButtons, function (btn) {
			btn.setAttribute("aria-pressed", btn.getAttribute("data-lang") === lang ? "true" : "false");
		});
		renderCapTable();
		setBridgeState(hasBridge());
		if (outEl.textContent === "" || !runStarted) {
			outEl.textContent = t("prompt");
		}
		try {
			window.localStorage.setItem(LANG_KEY, lang);
		} catch (e) {
			/* storage can be unavailable inside the WebView sandbox */
		}
	}


	function setBridgeState(available) {
		if (!available) {
			bridgeEl.className = "banner warn";
			bridgeEl.textContent = t("bridgeMissing");
		} else {
			bridgeEl.className = "banner ok";
			bridgeEl.textContent = t("bridgeOk");
		}
		Array.prototype.forEach.call(cmdButtons, function (btn) {
			btn.disabled = !available || busy;
		});
		if (clearButton) {
			clearButton.disabled = false;
		}
	}

	function normalizeResult(res) {
		var normal = { stdout: "", stderr: "", code: null };
		if (typeof res === "string") {
			normal.stdout = res;
			return normal;
		}
		if (res && typeof res === "object") {
			if (typeof res.stdout === "string") {
				normal.stdout = res.stdout;
			}
			if (typeof res.stderr === "string") {
				normal.stderr = res.stderr;
			}
			if (typeof res.errno === "number") {
				normal.code = res.errno;
			} else if (typeof res.code === "number") {
				normal.code = res.code;
			} else if (typeof res.errno === "string" && res.errno !== "") {
				var parsed = Number(res.errno);
				if (isFinite(parsed)) {
					normal.code = parsed;
				}
			}
			if (!("stdout" in res) && !("stderr" in res)) {
				try {
					normal.stdout = JSON.stringify(res);
				} catch (e) {
					normal.stdout = String(res);
				}
			}
			return normal;
		}
		if (typeof res !== "undefined" && res !== null) {
			normal.stdout = String(res);
		}
		return normal;
	}

	function formatResult(cmd, res) {
		var r = normalizeResult(res);
		var lines = [];
		lines.push("$ " + cmd);
		lines.push(t("exitCode") + ": " + (r.code === null ? t("unknown") : String(r.code)));
		lines.push("--- " + t("stdoutLabel") + " ---");
		lines.push(r.stdout === "" ? t("empty") : r.stdout);
		lines.push("--- " + t("stderrLabel") + " ---");
		lines.push(r.stderr === "" ? t("empty") : r.stderr);
		return lines.join("\n");
	}

	function runCommand(sub) {
		if (busy) {
			writeOut(t("busy"));
			return;
		}
		if (!hasBridge()) {
			writeOut(t("bridgeDenied"));
			setBridgeState(false);
			return;
		}
		var cmd = TOOL + " " + sub;
		runStarted = true;
		writeOut("$ " + cmd + "\n" + t("running"));
		busy = true;
		setBridgeState(true);

		var pending;
		try {
			pending = Promise.resolve(window.ksu.exec(cmd, { cwd: MODDIR }));
		} catch (err) {
			busy = false;
			setBridgeState(true);
			writeOut("$ " + cmd + "\n" + t("execFailed") + ": " + describeError(err));
			return;
		}
		pending.then(function (res) {
			writeOut(formatResult(cmd, res));
		}, function (err) {
			writeOut("$ " + cmd + "\n" + t("execFailed") + ": " + describeError(err));
		}).then(function () {
			busy = false;
			setBridgeState(hasBridge());
		});
	}

	function commandFor(button) {
		var base = button.getAttribute("data-cmd");
		if (base === "load" || base === "unload") {
			return base + " " + featureEl.value;
		}
		if (base === "profile") {
			return base + " " + profileEl.value;
		}
		return base;
	}

	Array.prototype.forEach.call(cmdButtons, function (btn) {
		btn.addEventListener("click", function () {
			runCommand(commandFor(btn));
		});
	});

	Array.prototype.forEach.call(langButtons, function (btn) {
		btn.addEventListener("click", function () {
			applyLang(btn.getAttribute("data-lang"));
		});
	});

	if (clearButton) {
		clearButton.addEventListener("click", function () {
			runStarted = false;
			writeOut(t("prompt"));
		});
	}

	var saved = null;
	try {
		saved = window.localStorage.getItem(LANG_KEY);
	} catch (e) {
		saved = null;
	}
	if (saved === "zh" || saved === "en") {
		lang = saved;
	} else {
		var navLang = (navigator.language || "").toLowerCase();
		lang = navLang.indexOf("zh") === 0 ? "zh" : "en";
	}
	applyLang(lang);
})();
