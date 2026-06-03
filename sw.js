// ╔══════════════════════════════════════════════════════════════╗
// ║  NEXUS Security — Service Worker                            ║
// ║  Handles: offline cache, push notifications                 ║
// ╚══════════════════════════════════════════════════════════════╝

const CACHE_NAME = 'nexus-v1';

// Files to cache for offline use
const CACHE_FILES = [
  '/nexus-security/',
  '/nexus-security/index.html',
  'https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Exo+2:wght@300;400;600;700&display=swap',
  'https://cdnjs.cloudflare.com/ajax/libs/paho-mqtt/1.1.0/paho-mqtt.min.js'
];

// ── Install: cache all static files ───────────────────────────
self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE_NAME).then(cache => {
      // Cache what we can — failures on external URLs are non-fatal
      return Promise.allSettled(
        CACHE_FILES.map(url => cache.add(url).catch(() => {}))
      );
    })
  );
  self.skipWaiting();
});

// ── Activate: clean up old caches ─────────────────────────────
self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    )
  );
  self.clients.claim();
});

// ── Fetch: serve from cache, fall back to network ─────────────
self.addEventListener('fetch', event => {
  // Skip MQTT WebSocket requests — never cache those
  if (event.request.url.includes('hivemq') || event.request.url.includes('mqtt')) return;

  event.respondWith(
    caches.match(event.request).then(cached => {
      return cached || fetch(event.request).then(response => {
        // Cache successful GET responses
        if (event.request.method === 'GET' && response.status === 200) {
          const clone = response.clone();
          caches.open(CACHE_NAME).then(cache => cache.put(event.request, clone));
        }
        return response;
      });
    }).catch(() => {
      // Full offline fallback — return cached index.html
      return caches.match('/nexus-security/index.html');
    })
  );
});

// ── Push notifications ─────────────────────────────────────────
// Triggered when the ESP32 publishes an alarm via MQTT
// The dashboard calls showNotification() which goes through here
self.addEventListener('push', event => {
  const data = event.data ? event.data.json() : {};
  const title = data.title || 'NEXUS Security';
  const options = {
    body:    data.body    || 'Alert triggered',
    icon:    '/nexus-security/icon-192.png',
    badge:   '/nexus-security/icon-192.png',
    vibrate: [200, 100, 200, 100, 200],
    tag:     'nexus-alert',       // replaces previous notification instead of stacking
    renotify: true,
    data:    { url: '/nexus-security/' },
    actions: [
      { action: 'disarm', title: 'Disarm' },
      { action: 'view',   title: 'Open dashboard' }
    ]
  };
  event.waitUntil(self.registration.showNotification(title, options));
});

// ── Notification click ─────────────────────────────────────────
self.addEventListener('notificationclick', event => {
  event.notification.close();

  if (event.action === 'disarm') {
    // Tell the dashboard to send DISARM command
    event.waitUntil(
      self.clients.matchAll({ type: 'window' }).then(clients => {
        if (clients.length > 0) {
          clients[0].postMessage({ action: 'DISARM' });
          clients[0].focus();
        } else {
          self.clients.openWindow('/nexus-security/');
        }
      })
    );
  } else {
    // Open or focus the dashboard
    event.waitUntil(
      self.clients.matchAll({ type: 'window' }).then(clients => {
        const match = clients.find(c => c.url.includes('nexus-security'));
        if (match) { match.focus(); }
        else { self.clients.openWindow('/nexus-security/'); }
      })
    );
  }
});
