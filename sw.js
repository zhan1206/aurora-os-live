// COI Service Worker — enables SharedArrayBuffer on GitHub Pages
// GitHub Pages won't send COOP/COEP headers, so this SW adds them.
// visit https://zhan1206.github.io/aurora-os-live/ — first load auto-refreshes
'use strict';

var HEADERS = {
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp'
};

self.addEventListener('install', function() {
    self.skipWaiting();
});

self.addEventListener('activate', function(event) {
    event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', function(event) {
    if (event.request.method !== 'GET') return;
    try {
        event.respondWith(
            fetch(event.request).then(function(response) {
                if (response.status === 0) return response;
                var headers = new Headers(response.headers);
                Object.keys(HEADERS).forEach(function(k) {
                    headers.set(k, HEADERS[k]);
                });
                return new Response(response.body, {
                    status: response.status,
                    statusText: response.statusText,
                    headers: headers
                });
            }).catch(function(e) {
                console.warn('coi-sw fetch error:', e);
                return fetch(event.request);
            })
        );
    } catch (e) {
        // pass through
    }
});
