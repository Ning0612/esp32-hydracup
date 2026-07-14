(function () {
  'use strict';

  var THEME_KEY = 'iot-ui-theme';
  var csrfToken = '';

  function isTheme(value) {
    return value === 'light' || value === 'dark';
  }

  function syncThemeButtons() {
    var current = document.documentElement.dataset.theme;
    document.querySelectorAll('[data-theme-choice]').forEach(function (button) {
      var active = button.getAttribute('data-theme-choice') === current;
      button.classList.toggle('active', active);
      button.setAttribute('aria-pressed', active ? 'true' : 'false');
    });
  }

  function setTheme(theme) {
    if (!isTheme(theme)) return;
    document.documentElement.dataset.theme = theme;
    try { localStorage.setItem(THEME_KEY, theme); } catch (e) {}
    syncThemeButtons();
    window.dispatchEvent(new CustomEvent('hydra-theme-change'));
  }

  function redirectToLogin() {
    if (location.pathname === '/login') return;
    var next = location.pathname + location.search + location.hash;
    location.replace('/login?next=' + encodeURIComponent(next));
  }

  var authReady = fetch('/api/auth/csrf', { cache: 'no-store' })
    .then(function (response) { return response.ok ? response.json() : {}; })
    .then(function (data) {
      csrfToken = typeof data.csrf === 'string' ? data.csrf : '';
      window.hydraAuth = data;
      return data;
    })
    .catch(function () {
      window.hydraAuth = {};
      return {};
    });

  window.hydraUiReady = authReady;
  window.hydraFetch = function (url, options) {
    options = options || {};
    return authReady.then(function () {
      var headers = new Headers(options.headers || {});
      var method = (options.method || 'GET').toUpperCase();
      if (method !== 'GET' && method !== 'HEAD' && csrfToken)
        headers.set('X-CSRF-Token', csrfToken);
      options.headers = headers;
      return fetch(url, options);
    }).then(function (response) {
      if (response.status === 401 && url.indexOf('/api/auth/login') !== 0)
        redirectToLogin();
      return response;
    });
  };

  document.addEventListener('DOMContentLoaded', function () {
    syncThemeButtons();
    document.querySelectorAll('[data-theme-choice]').forEach(function (button) {
      button.addEventListener('click', function () {
        setTheme(button.getAttribute('data-theme-choice'));
      });
    });
    document.querySelectorAll('[data-logout]').forEach(function (button) {
      button.addEventListener('click', function () {
        button.disabled = true;
        window.hydraFetch('/api/auth/logout', { method: 'POST' })
          .then(function () { location.replace('/login'); })
          .catch(function () { location.replace('/login'); });
      });
    });
  });
})();
