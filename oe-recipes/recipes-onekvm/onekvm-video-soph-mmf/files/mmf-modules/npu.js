"use strict";
const I = {
  default: {
    title: "NPU",
    runtime: "OCR runtime",
    module: "Kernel module",
    device: "Device",
    available: "Device present",
    basedOn: "Based on",
    yes: "Yes",
    no: "No",
    unknown: "Unknown",
    unavailable: "Status unavailable",
  },
  zh: {
    title: "NPU",
    runtime: "OCR 运行时",
    module: "内核模块",
    device: "设备节点",
    available: "设备已出现",
    basedOn: "基于",
    yes: "是",
    no: "否",
    unknown: "未知",
    unavailable: "无法读取状态",
  },
};
const localeOf = (value) => {
  const language = String(value || "").replace(/_/g, "-").toLowerCase();
  if (language === "zh-tw" || language === "zh-hk" || language === "zh-mo" || language.includes("hant"))
    return "zh";
  if (language === "zh" || language.startsWith("zh-")) return "zh";
  return "default";
};
let hostLanguage = "";
try {
  hostLanguage = localStorage.getItem("nano-kvm-language") || "";
} catch {}
const locale = (hostLanguage && localeOf(hostLanguage)) || localeOf(navigator.language) || "default";
const t = I[locale] || I.default;
document.documentElement.lang = locale === "zh" ? "zh-CN" : "en";
document.querySelectorAll("[data-t]").forEach((element) => {
  element.textContent = t[element.dataset.t];
});
document.title = t.title;

function text(id, value) {
  document.getElementById(id).textContent = value || t.unknown;
}

async function refresh() {
  try {
    const response = await fetch("/api/status", { headers: { Accept: "application/json" } });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const status = await response.json();
    const ocr = status.ocr || {};
    const runtime = ocr.runtime || t.unknown;
    const module = ocr.module || t.unknown;
    const device = ocr.device || t.unknown;
    text("runtime", runtime);
    text("module", module);
    text("device", device);
    text("available", ocr.available ? t.yes : t.no);
    const parts = [module, runtime].filter((value) => value && value !== t.unknown);
    document.getElementById("backends").textContent = `${t.basedOn} ${parts.join(" · ") || t.unknown}`;
  } catch (error) {
    document.getElementById("backends").textContent = t.unavailable;
    text("runtime", t.unavailable);
    text("module", t.unavailable);
    text("device", t.unavailable);
    text("available", t.unavailable);
  }
}

refresh();
