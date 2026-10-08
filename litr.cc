// ==UserScript==
// @name         Listen Trackers to Torrent Upload
// @namespace    http://tampermonkey.net/
// @version      1.20
// @description  Кнопка отправки magnet-ссылки на локальный torrent-сервер (litr.cc)
// @author       Chuvakstepan, DeepSeek
// @match        https://litr.cc/*
// @grant        GM_xmlhttpRequest
// @connect      192.168.1.133
// @connect      litr.cc
// @connect      rutracker.org
// @connect      rutracker.net
// @connect      fastpic.org
// @connect      i128.fastpic.org
// @connect      i127.fastpic.org
// @connect      i4.imageban.ru
// @connect      images4.imagebam.com
// @connect      postimg.cc
// @connect      i.imgur.com
// @run-at       document-end
// ==/UserScript==

(function() {
    'use strict';

    console.log('%c[LITR] Listen Trackers Uploader v1.20', 'color:#0a0;font-weight:bold');

    const DEBUG = true;
    const USE_MAGNET = true;
    const LOCAL_POSTER_SCRIPT = 'http://192.168.1.133/postimg.php';
    const TORRENT_SERVER_URL   = 'http://192.168.1.133:8090';

    const log  = (...a) => DEBUG && console.log('[LITR]', ...a);
    const warn = (...a) => console.warn('[LITR]', ...a);

    // ==============================
    //  Чтение данных со страницы
    // ==============================

    function getTorrentTitle() {
        const h1 = document.querySelector('h1');
        if (h1 && h1.textContent.trim()) return h1.textContent.trim();
        return (document.title || '').replace(/\s*-\s*Раздача\s*-\s*Listen Trackers\s*$/i, '').trim();
    }

    function getMagnetLink() {
        const a = document.querySelector('a[href^="magnet:?xt=urn:btih:"]');
        return a ? a.href : null;
    }

    function getDownloadUrl() {
        const a = document.querySelector('a[href*="dl.php?t="]');
        return a ? a.href : null;
    }

    function getTorrentId() {
        const u = getDownloadUrl();
        if (!u) return null;
        try { return new URL(u).searchParams.get('t'); } catch { return null; }
    }

    /**
     * Возвращает { original, proxy }.
     *   original — реальный URL картинки (fastpic и т.п.)
     *   proxy    — URL через imageProxy litr.cc (если был)
     */
    function getPosterUrls() {
        const out = { original: '', proxy: '' };
        // ищем ЛЮБОЙ <img>, у которого в src есть imageProxy
        const imgs = document.querySelectorAll('img');
        for (const img of imgs) {
            const src = img.src || '';
            if (!src.includes('imageProxy')) continue;
            out.proxy = src;
            try {
                const u = new URL(src);
                const orig = u.searchParams.get('url');
                if (orig && orig.startsWith('http')) out.original = orig;
            } catch (e) {
                warn('imageProxy parse:', e.message);
            }
            log('Найден imageProxy:', out);
            return out;
        }
        // fallback — ищем по известным хостам
        const hosts = ['fastpic','imageban','imagebam','postimg','imgur','images4','radikal'];
        for (const img of imgs) {
            const src = img.src || '';
            if (!src.startsWith('http')) continue;
            if (hosts.some(h => src.toLowerCase().includes(h))) {
                out.original = src;
                log('Fallback постер:', src);
                return out;
            }
        }
        return out;
    }

    function findTorrentFileLink() {
        for (const a of document.querySelectorAll('a')) {
            if (a.textContent.trim() === 'Torrent-файл') return a;
        }
        return null;
    }

    // ==============================
    //  Сеть
    // ==============================

    function httpGet(url, opts = {}) {
        return new Promise((resolve, reject) => {
            GM_xmlhttpRequest({
                method: 'GET',
                url,
                responseType: opts.responseType || 'text',
                withCredentials: opts.withCredentials || false,
                headers: opts.headers || {},
                onload: r => resolve(r),
                onerror: () => reject(new Error('network')),
                ontimeout: () => reject(new Error('timeout')),
                timeout: opts.timeout || 30000
            });
        });
    }

    /**
     * Пробует залить постер: сначала imageProxy (litr.cc отдаёт с правильным Referer
     * на fastpic), потом original. Возвращает итоговый URL или '' при провале.
     */
    async function uploadPoster(posterUrls) {
        const { original, proxy } = posterUrls;
        if (!original && !proxy) {
            warn('Нет URL постера ни в одном варианте');
            return '';
        }

        const tryUrl = async (label, u) => {
            if (!u) return '';
            const req = LOCAL_POSTER_SCRIPT + '?url=' + encodeURIComponent(u);
            log(`postimg ← ${label}:`, u);
            try {
                const r = await httpGet(req, { timeout: 25000 });
                log(`postimg [${label}] status=${r.status} resp=${(r.responseText||'').slice(0,300)}`);
                if (r.status === 200) {
                    try {
                        const j = JSON.parse(r.responseText);
                        if (j.success && j.url) return j.url;
                        warn(`postimg вернул ошибку:`, j.error || j);
                    } catch (e) {
                        warn('JSON parse:', e.message, r.responseText);
                    }
                }
            } catch (e) {
                warn(`postimg [${label}] ошибка:`, e.message);
            }
            return '';
        };

        // сначала proxy
        let r = await tryUrl('proxy', proxy);
        if (r) return r;
        // потом оригинал
        r = await tryUrl('original', original);
        if (r) return r;
        return '';
    }

    function uploadMagnet(magnetLink, title, posterUrl) {
        return new Promise((resolve, reject) => {
            const payload = {
                action: 'add',
                link: magnetLink,
                title: title || '',
                poster: posterUrl || '',
                save_to_db: true
            };
            log('POST /torrents payload:', payload);
            GM_xmlhttpRequest({
                method: 'POST',
                url: TORRENT_SERVER_URL + '/torrents',
                headers: { 'Content-Type': 'application/json', 'Accept': 'application/json' },
                data: JSON.stringify(payload),
                onload: r => {
                    log('Сервер статус:', r.status, 'ответ:', r.responseText);
                    if (r.status === 200 || r.status === 201) {
                        try { resolve(JSON.parse(r.responseText)); }
                        catch { resolve(r.responseText); }
                    } else reject(new Error('HTTP ' + r.status));
                },
                onerror: () => reject(new Error('network')),
                ontimeout: () => reject(new Error('timeout')),
                timeout: 30000
            });
        });
    }

    // ==============================
    //  UI
    // ==============================

    function makeTitleFromData() {
        const poster = getPosterUrls();
        const id = getTorrentId();
        const mg = getMagnetLink();
        return  `Poster original: ${poster.original || '(пусто)'}\n` +
                `Poster proxy:    ${poster.proxy    || '(пусто)'}\n` +
                `Torrent id:      ${id || '(пусто)'}\n` +
                `Magnet:          ${mg ? 'есть' : 'нет'}`;
    }

    function addUploadButton() {
        const existing = document.getElementById('litr-upload-container');
        if (existing) {
            // кнопка уже есть — просто обновим tooltip (постер мог догрузиться)
            existing.title = makeTitleFromData();
            return;
        }

        const torrentFileLink = findTorrentFileLink();
        if (!torrentFileLink) return;

        const container = torrentFileLink.parentElement;
        if (!container) return;

        const btn = document.createElement('a');
        btn.id = 'litr-upload-container';
        btn.href = 'javascript:void(0)';
        btn.title = makeTitleFromData();
        btn.style.cssText =
            'cursor:pointer;user-select:none;text-decoration:none;color:inherit;' +
            'display:inline-flex;flex-direction:column;align-items:center;gap:4px;';

        btn.innerHTML = `
            <span role="img" class="anticon" style="font-size:1em;line-height:0;">
                <svg viewBox="64 64 896 896" width="1em" height="1em" fill="currentColor" aria-hidden="true" focusable="false">
                    <path d="M400 317.7h73.9V656c0 4.4 3.6 8 8 8h60c4.4 0 8-3.6 8-8V317.7H624c6.7 0 10.4-7.7 6.3-12.9L518.3 163a8 8 0 00-12.6 0l-112 141.7c-4.1 5.3-.4 13 6.3 13zM878 626h-60c-4.4 0-8 3.6-8 8v154H214V634c0-4.4-3.6-8-8-8h-60c-4.4 0-8 3.6-8 8v198c0 17.7 14.3 32 32 32h684c17.7 0 32-14.3 32-32V634c0-4.4-3.6-8-8-8z"/>
                </svg>
            </span>
            <div id="litr-upload-btn-text">Отправить</div>
        `;

        container.appendChild(btn);
        log('Кнопка добавлена');

        btn.addEventListener('click', async (e) => {
            e.preventDefault();
            const textEl = document.getElementById('litr-upload-btn-text');
            if (!textEl) return;
            const reset = () => { textEl.textContent = 'Отправить'; };

            btn.style.pointerEvents = 'none';
            btn.style.opacity = '0.6';

            try {
                // ВАЖНО: всё читаем заново при клике — данные точно актуальны
                const title    = getTorrentTitle();
                const magnet   = getMagnetLink();
                const poster   = getPosterUrls();

                log('Клик. title=', title);
                log('Клик. magnet=', magnet);
                log('Клик. poster=', poster);

                if (!magnet) throw new Error('Magnet-ссылка не найдена');

                textEl.textContent = '⏳ Постер...';
                const posterUrl = await uploadPoster(poster);

                if (!posterUrl) {
                    warn('Постер не загружен, продолжаем без него');
                } else {
                    log('Постер загружен →', posterUrl);
                }

                textEl.textContent = '⏳ Отправка...';
                const res = await uploadMagnet(magnet, title, posterUrl);
                log('Готово:', res);

                textEl.textContent = '✅ Готово';
                setTimeout(reset, 3000);
            } catch (err) {
                warn('Ошибка:', err);
                textEl.textContent = '❌ ' + (err.message || 'Ошибка').slice(0, 30);
                setTimeout(reset, 4000);
            } finally {
                btn.style.pointerEvents = '';
                btn.style.opacity = '';
            }
        });
    }

    // ==============================
    //  Отладка
    // ==============================

    window.litrDebug = function() {
        console.log('=== litrDebug ===');
        console.log('Title   :', getTorrentTitle());
        console.log('Magnet  :', getMagnetLink());
        console.log('Torrent :', getDownloadUrl());
        console.log('ID      :', getTorrentId());
        console.log('Poster  :', getPosterUrls());
        console.log('Все img на странице:');
        document.querySelectorAll('img').forEach((img, i) => {
            console.log(`  [${i}] ${img.src}`);
        });
    };

    window.litrTestPoster = async function() {
        const poster = getPosterUrls();
        console.log('posterUrls =', poster);
        if (!poster.original && !poster.proxy) {
            console.warn('Нечего заливать');
            return;
        }
        const r = await uploadPoster(poster);
        console.log('Результат uploadPoster:', r);
    };

    // ==============================
    //  Init
    // ==============================

    function init() {
        log('init; USE_MAGNET =', USE_MAGNET);
        addUploadButton();
        setTimeout(addUploadButton, 800);
        setTimeout(addUploadButton, 2000);
        setTimeout(addUploadButton, 5000);
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else init();

    new MutationObserver(() => addUploadButton())
        .observe(document.body, { childList: true, subtree: true });

})();
