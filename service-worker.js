// service-worker.js
// Battery Balancing System PWA Service Worker
// Bypasses network cache and updates on reload

const CACHE_NAME = 'battery-monitor-v3';
const OFFLINE_URL = '/offline.html';

// Files to cache (always cache latest version)
const urlsToCache = [
  '/',
  '/index.html',
  '/offline.html',
  '/manifest.json',
  'https://fonts.googleapis.com/css2?family=Inter:wght@300;400;500;600;700;800&display=swap',
  'https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js',
  'https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.0.0-beta3/css/all.min.css',
  'https://www.gstatic.com/firebasejs/9.22.0/firebase-app-compat.js',
  'https://www.gstatic.com/firebasejs/9.22.0/firebase-database-compat.js'
];

// Install event - cache files
self.addEventListener('install', event => {
  console.log('[SW] Installing...');
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then(cache => {
        console.log('[SW] Caching app shell');
        return cache.addAll(urlsToCache);
      })
      .then(() => self.skipWaiting())
  );
});

// Activate event - clean old caches and take control
self.addEventListener('activate', event => {
  console.log('[SW] Activating...');
  event.waitUntil(
    caches.keys().then(cacheNames => {
      return Promise.all(
        cacheNames.map(cache => {
          if (cache !== CACHE_NAME) {
            console.log('[SW] Deleting old cache:', cache);
            return caches.delete(cache);
          }
        })
      );
    }).then(() => {
      console.log('[SW] Claiming clients');
      return self.clients.claim();
    })
  );
});

// Fetch event - NETWORK FIRST strategy with always update
// This ensures we get latest data but fallback to cache when offline
self.addEventListener('fetch', event => {
  const url = event.request.url;
  
  // Skip Firebase and external APIs (they handle their own caching)
  if (url.includes('firebaseio.com') || 
      url.includes('googleapis.com') ||
      url.includes('gstatic.com') ||
      url.includes('cloudflare.com') ||
      url.includes('cdn.jsdelivr.net') ||
      url.includes('cdnjs.cloudflare.com')) {
    // Network first for external APIs (no caching)
    event.respondWith(
      fetch(event.request)
        .catch(() => {
          // Return a simple response for offline
          return new Response(JSON.stringify({ error: 'Offline' }), {
            headers: { 'Content-Type': 'application/json' }
          });
        })
    );
    return;
  }
  
  // For HTML and app files - NETWORK FIRST, fallback to cache
  event.respondWith(
    fetch(event.request)
      .then(response => {
        // Clone the response
        const responseToCache = response.clone();
        // Update cache with new version
        caches.open(CACHE_NAME)
          .then(cache => {
            cache.put(event.request, responseToCache);
          });
        return response;
      })
      .catch(() => {
        // If network fails, try cache
        return caches.match(event.request)
          .then(response => {
            if (response) {
              return response;
            }
            // If requesting HTML page, return offline page
            if (event.request.headers.get('accept').includes('text/html')) {
              return caches.match(OFFLINE_URL);
            }
            return new Response('Offline content not available', {
              status: 503,
              statusText: 'Service Unavailable'
            });
          });
      })
  );
});

// Message listener for skipWaiting
self.addEventListener('message', event => {
  if (event.data === 'skipWaiting') {
    self.skipWaiting();
  }
});