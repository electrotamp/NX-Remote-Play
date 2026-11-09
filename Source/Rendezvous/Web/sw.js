'use strict';
// Pass-through worker. Chrome on Android may require a registered worker with a fetch handler before it offers to install the
// page as an app. It caches nothing and answers nothing itself, so every request still goes to the server (no stale page,
// no interference with the stream's signalling).
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (event) => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', () => {});
