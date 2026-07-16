// coi-serviceworker - adds COOP/COEP headers for SharedArrayBuffer support
// From: https://github.com/gzuidhof/coi-serviceworker (CC0/public domain)
"use strict";
(() => {
  if (typeof window === "undefined") {
    const SH = {
      "Cross-Origin-Embedder-Policy": "require-corp",
      "Cross-Origin-Opener-Policy": "same-origin",
    };
    self.addEventListener("install", () => self.skipWaiting());
    self.addEventListener("activate", (e) => e.waitUntil(self.clients.claim()));
    self.addEventListener("fetch", (e) => {
      e.respondWith(
        (async () => {
          const r = await fetch(e.request);
          if (r.status === 0) return r;
          const h = new Headers(r.headers);
          Object.entries(SH).forEach(([k, v]) => h.set(k, v));
          return new Response(r.body, {
            status: r.status,
            statusText: r.statusText,
            headers: h,
          });
        })()
      );
    });
  } else {
    if (window.crossOriginIsolated) return;
    navigator.serviceWorker
      ?.register(document.currentScript?.src)
      .then((r) => {
        r.active && location.reload();
        r.addEventListener("updatefound", () => {
          const w = r.installing;
          w.addEventListener("statechange", () => w.state === "activated" && location.reload());
        });
      }, console.error);
  }
})();
