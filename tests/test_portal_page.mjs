// Browser-script regression checks using only synthetic DOM/HTTP fixtures.
// Run with Node.js: node tests/test_portal_page.mjs. No device or real HTTP access.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

const html = await readFile(new URL('../main/portal.html', import.meta.url), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

class Element {
    constructor(tagName, attributes = {}) {
        this.tagName = tagName;
        this.attributes = attributes;
        this.id = attributes.id || '';
        this._value = attributes.value || '';
        this.disabled = 'disabled' in attributes;
        this.required = 'required' in attributes;
        this.checked = 'checked' in attributes;
        this.hidden = 'hidden' in attributes;
        this.readOnly = 'readonly' in attributes;
        this.type = attributes.type || 'text';
        this.placeholder = attributes.placeholder || '';
        this.textContent = '';
        this.dataset = {};
        this.handlers = {};
        this.options = [];
        for (const [key, value] of Object.entries(attributes)) {
            if (key.startsWith('data-')) this.dataset[key.slice(5)] = value;
        }
    }
    get value() { return this._value; }
    set value(value) {
        // A real select cannot keep a value absent from its options.
        this._value = this.tagName === 'select' && !this.options.some(o => o.value === value)
            ? '' : value;
    }
    get selectedOptions() { return this.options.filter(o => o.value === this._value); }
    addEventListener(event, fn) { this.handlers[event] = fn; }
    replaceChildren(...children) {
        this.options = children;
        this._value = children[0]?.value || '';
    }
    add(option) {
        this.options.push(option);
        if (this.options.length === 1) this._value = option.value;
    }
}
class Option extends Element {
    constructor(text, value) {
        super('option', {value});
        this.text = this.textContent = text;
    }
}
const networks = [
    {ssid:'Fixture WiFi', rssi:-65, auth:3},
    {ssid:'Fixture Open', rssi:-50, auth:0},
    {ssid:'<text-not-markup>', rssi:-70, auth:3},
    {ssid:'Fixture WiFi', rssi:-35, auth:3},
    {ssid:'Fixture WiFi', rssi:-55, auth:0},
    {ssid:'', rssi:-10, auth:3}
];
function fixture({patSet = false, savedSsid = 'Fixture WiFi', busy = false,
    done = false, scanReply = networks, initialStatusError = null} = {}) {
    const nodes = {}, toggles = [];
    // Load real markup attributes so tests do not invent required/hidden defaults.
    for (const [, tag, source] of html.matchAll(/<([a-z][\w-]*)\b([^>]*)>/gi)) {
        const attributes = Object.fromEntries([...source.matchAll(/([^\s=]+)(?:="([^"]*)")?/g)]
            .map(([, key, value]) => [key, value ?? '']));
        if (!attributes.id && !attributes['data-toggle']) continue;
        const node = new Element(tag, attributes);
        if (attributes.id) nodes[attributes.id] = node;
        if (attributes['data-toggle']) toggles.push(node);
    }
    nodes.networks.add(new Option('请选择检测到的 Wi-Fi', ''));
    nodes.scan.textContent = '刷新 Wi-Fi 列表';
    const controls = Object.values(nodes).filter(n => ['input','button','select'].includes(n.tagName))
        .concat(toggles);
    const state = {stage:1, busy, done, pat_set:patSet, ssid:savedSsid,
        token:'synthetic-csrf-value', message:'ready'};
    const requests = [], timers = [];
    const queues = {'/status':initialStatusError ? [initialStatusError] : [], '/scan':[], '/save':[]};
    let currentScan = scanReply, saveReply = {message:'accepted'};
    const context = vm.createContext({
        document: {
            getElementById: id => {
                assert.ok(nodes[id], `Unknown DOM id: ${id}`);
                return nodes[id];
            },
            querySelectorAll: selector => {
                if (selector === '[data-toggle]') return toggles;
                assert.equal(selector, '#config input,#config button,#config select');
                return controls;
            }
        },
        Option, URLSearchParams,
        setTimeout: (callback, delay) => { timers.push({callback, delay}); return timers.length; },
        fetch: async (url, options) => {
            assert.ok(url in queues, `Unexpected endpoint: ${url}`);
            requests.push({url, options});
            const reply = await (queues[url].length ? queues[url].shift()
                : url === '/status' ? {...state} : url === '/scan' ? currentScan : saveReply);
            if (reply instanceof Error) throw reply;
            if (reply && 'ok' in reply) return {ok:reply.ok, json:async () => reply.body};
            return {ok:true, json:async () => reply};
        }
    });
    vm.runInContext(script, context);
    return {nodes, state, requests, context, controls, toggles, timers,
        setScan: reply => currentScan = reply,
        enqueue: (url, reply) => queues[url].push(reply),
        setError: error => saveReply = error ? {ok:false, body:{message:error}} : {message:'accepted'}};
}
function deferred() {
    let resolve;
    const promise = new Promise(r => resolve = r);
    return {promise, resolve};
}
async function tick() { for (let i = 0; i < 20; i++) await Promise.resolve(); }
const count = (f, url) => f.requests.filter(r => r.url === url).length;
const poll = f => vm.runInContext('poll()', f.context);
const refresh = f => f.nodes.scan.handlers.click();
const submit = f => f.nodes.config.handlers.submit({preventDefault(){}});
function select(f, ssid, auth = 3) {
    const option = f.nodes.networks.options.find(o => o.value === JSON.stringify([ssid, auth]));
    assert.ok(option, `Missing synthetic network: ${ssid}/${auth}`);
    f.nodes.networks.value = option.value;
    f.nodes.networks.handlers.change();
}
function manual(f, checked = true) {
    f.nodes.hiddenNetwork.checked = checked;
    f.nodes.hiddenNetwork.handlers.change();
}
function locked(f) { assert.ok(f.controls.every(e => e.disabled), 'All form controls must stay locked'); }
function lastBody(f) {
    return new URLSearchParams(f.requests.findLast(r => r.url === '/save').options.body);
}
let passed = 0;
async function test(name, run) {
    await run();
    passed++;
    console.log(`PASS: ${name}`);
}

await test('automatic scan once, strongest-first deduplication, saved SSID and safe text', async () => {
    const f = fixture();
    locked(f);
    assert.equal(f.nodes.ssid.required, false);
    assert.equal(f.nodes.ssid.readOnly, true);
    assert.equal(f.nodes.ssid.hidden, true);
    assert.equal(f.nodes.manualNetwork.hidden, true);
    assert.equal(f.nodes.networks.required, true);
    await tick();
    assert.equal(count(f, '/scan'), 1);
    assert.equal(f.nodes.pat.required, true);
    assert.equal(f.nodes.ssid.value, 'Fixture WiFi');
    assert.equal(f.nodes.networks.value, JSON.stringify(['Fixture WiFi', 3]));
    assert.equal(f.nodes.status.textContent, 'ready');
    assert.equal(f.nodes.networks.options.length, 5); // placeholder + four unique SSID/auth pairs
    assert.deepEqual(f.nodes.networks.options.slice(1).map(o => o.dataset.ssid),
        ['Fixture WiFi','Fixture Open','Fixture WiFi','<text-not-markup>']);
    assert.match(f.nodes.networks.options[1].text, /-35 dBm/);
    assert.equal(f.nodes.networks.options[4].textContent, '<text-not-markup> · -70 dBm · 加密 (3)');
    assert.equal(f.nodes.scanStatus.attributes['aria-live'], 'polite');
    assert.equal(f.nodes.scan.textContent, '刷新 Wi-Fi 列表');
    await poll(f); await poll(f);
    assert.equal(count(f, '/scan'), 1);
    assert.ok(f.timers.every(t => t.delay === 1500));
});

await test('no unseen saved SSID or unsaved first-network selection', async () => {
    for (const savedSsid of ['Fixture Missing', '']) {
        const f = fixture({savedSsid}); await tick();
        assert.equal(f.nodes.networks.value, '');
        assert.equal(f.nodes.ssid.value, '');
        assert.equal(f.nodes.pass.disabled, true);
        await submit(f);
        assert.equal(count(f, '/save'), 0);
        assert.match(f.nodes.status.textContent, /请选择检测到/);
    }
});

await test('status failure/busy defers initialization scan until ready', async () => {
    const f = fixture({initialStatusError:new Error('synthetic offline')}); await tick();
    assert.equal(count(f, '/scan'), 0); locked(f);
    assert.match(f.nodes.status.textContent, /暂未连接设备/);
    f.state.busy = true; await poll(f); await tick();
    assert.equal(count(f, '/scan'), 0); locked(f);
    f.state.busy = false; await poll(f); await tick();
    assert.equal(count(f, '/scan'), 1);
    await poll(f); assert.equal(count(f, '/scan'), 1);
    const completed = fixture({done:true}); await tick();
    assert.equal(count(completed, '/scan'), 0); locked(completed);
});

await test('refresh retains exact SSID/auth and password despite reordered signals', async () => {
    const f = fixture({savedSsid:''}); await tick();
    select(f, 'Fixture WiFi'); f.nodes.pass.value = 'synthetic-old-password';
    const key = f.nodes.networks.value;
    f.setScan([{ssid:'Fixture WiFi', rssi:-80, auth:3}, {ssid:'Fixture Open', rssi:-20, auth:0}]);
    await refresh(f);
    assert.equal(f.nodes.networks.value, key);
    assert.equal(f.nodes.ssid.value, 'Fixture WiFi');
    assert.equal(f.nodes.pass.value, 'synthetic-old-password');
    assert.equal(f.nodes.open.checked, false);
    assert.equal(f.nodes.pass.disabled, false);
});

await test('disappearance clears selection/SSID/password without falling back to saved SSID', async () => {
    const f = fixture(); await tick();
    select(f, '<text-not-markup>'); f.nodes.pass.value = 'synthetic-old-password';
    f.setScan([{ssid:'Fixture WiFi', rssi:-30, auth:3}]); await refresh(f);
    assert.equal(f.nodes.networks.value, '');
    assert.equal(f.nodes.ssid.value, ''); assert.equal(f.nodes.pass.value, '');
    assert.match(f.nodes.scanStatus.textContent, /原选网络已不在列表/);
    f.nodes.ssid.value = '<text-not-markup>'; // even a stale/tampered mirror cannot be submitted
    await submit(f); assert.equal(count(f, '/save'), 0);
    await poll(f); assert.equal(f.nodes.ssid.value, '<text-not-markup>');
    await submit(f); assert.equal(count(f, '/save'), 0);
});

await test('SSID/auth changes and clearing selection discard old passwords; open networks disable password', async () => {
    const f = fixture(); await tick();
    f.nodes.pass.value = 'synthetic-old-password'; select(f, '<text-not-markup>');
    assert.equal(f.nodes.pass.value, ''); assert.equal(f.nodes.pass.disabled, false);
    f.nodes.pass.value = 'synthetic-old-password'; select(f, 'Fixture WiFi');
    assert.equal(f.nodes.pass.value, '');
    f.nodes.pass.value = 'synthetic-old-password'; select(f, 'Fixture WiFi', 0);
    assert.equal(f.nodes.pass.value, ''); assert.equal(f.nodes.pass.disabled, true);
    assert.equal(f.nodes.open.checked, true); assert.equal(f.nodes.open.disabled, true);
    await poll(f); assert.equal(f.nodes.pass.disabled, true);
    select(f, 'Fixture WiFi'); assert.equal(f.nodes.pass.disabled, false);
    f.nodes.pass.value = 'synthetic-old-password';
    f.nodes.networks.value = ''; f.nodes.networks.handlers.change();
    assert.equal(f.nodes.ssid.value, ''); assert.equal(f.nodes.pass.value, '');
});

await test('empty/hidden-only scans, HTTP failure and malformed results explain refresh/hidden options', async () => {
    for (const reply of [[], [{ssid:'', rssi:-20, auth:0}],
        {ok:false, body:{message:'synthetic scan unavailable'}}, new Error('synthetic fetch failure'), {}]) {
        const f = fixture(); await tick();
        f.setError('synthetic form error'); await submit(f);
        select(f, 'Fixture WiFi'); f.nodes.pass.value = 'synthetic-old-password';
        f.setScan(reply); await refresh(f);
        assert.equal(f.nodes.networks.options.length, 1);
        assert.equal(f.nodes.networks.value, ''); assert.equal(f.nodes.ssid.value, '');
        assert.equal(f.nodes.pass.value, '');
        assert.match(f.nodes.scanStatus.textContent, /刷新 Wi-Fi 列表/);
        assert.match(f.nodes.scanStatus.textContent, /连接隐藏网络（手动输入名称）/);
        assert.match(f.nodes.scanStatus.textContent, /未检测到|扫描失败/);
        assert.equal(f.nodes.status.textContent, 'synthetic form error');
        assert.equal(f.nodes.save.disabled, false); assert.equal(f.nodes.hiddenNetwork.disabled, false);
        await submit(f); assert.equal(count(f, '/save'), 1);
    }
});

await test('failed automatic scan can be manually retried; polling does not auto retry', async () => {
    const f = fixture({scanReply:{ok:false, body:{message:'synthetic unavailable'}}}); await tick();
    assert.match(f.nodes.scanStatus.textContent, /扫描失败：synthetic unavailable/);
    assert.equal(f.nodes.ssid.value, '');
    await poll(f); assert.equal(count(f, '/scan'), 1);
    f.setScan(networks); await refresh(f);
    assert.equal(f.nodes.ssid.value, 'Fixture WiFi');
    assert.equal(count(f, '/scan'), 2);
});

await test('hidden-network opt-in, required fields, open mode and return to ordinary selection', async () => {
    const f = fixture(); await tick(); f.nodes.pass.value = 'synthetic-old-password';
    manual(f);
    assert.equal(f.nodes.manualNetwork.hidden, false); assert.equal(f.nodes.ssid.hidden, false);
    assert.equal(f.nodes.ssid.readOnly, false); assert.equal(f.nodes.ssid.required, true);
    assert.equal(f.nodes.networks.required, false); assert.equal(f.nodes.networks.disabled, true);
    assert.equal(f.nodes.networks.value, ''); assert.equal(f.nodes.ssid.value, '');
    assert.equal(f.nodes.pass.value, ''); assert.equal(f.nodes.open.disabled, false);
    await submit(f); assert.equal(count(f, '/save'), 0);
    assert.match(f.nodes.status.textContent, /请输入隐藏网络/);
    f.nodes.ssid.value = 'Fixture Hidden'; f.nodes.ssid.handlers.input();
    f.nodes.pass.value = 'synthetic-hidden-password';
    f.setScan([]); await refresh(f);
    assert.equal(f.nodes.ssid.value, 'Fixture Hidden');
    assert.equal(f.nodes.pass.value, 'synthetic-hidden-password');
    assert.equal(f.nodes.networks.disabled, true);
    f.setScan(new Error('synthetic scan failure')); await refresh(f);
    assert.equal(f.nodes.ssid.value, 'Fixture Hidden');
    assert.equal(f.nodes.pass.value, 'synthetic-hidden-password');
    f.nodes.ssid.value = 'Fixture Other Hidden'; f.nodes.ssid.handlers.input();
    assert.equal(f.nodes.pass.value, '');
    f.nodes.pass.value = 'synthetic-hidden-password'; f.nodes.open.checked = true;
    f.nodes.open.handlers.change();
    assert.equal(f.nodes.pass.value, ''); assert.equal(f.nodes.pass.disabled, true);
    await poll(f); assert.equal(f.nodes.networks.disabled, true); assert.equal(f.nodes.pass.disabled, true);
    f.nodes.open.checked = false; f.nodes.open.handlers.change();
    assert.equal(f.nodes.pass.disabled, false);
    f.setError('synthetic validation'); f.nodes.pat.value = 'pt-synthetic-fixture-only';
    f.nodes.pass.value = 'synthetic-hidden-password'; await submit(f);
    assert.equal(lastBody(f).get('ssid'), 'Fixture Other Hidden');
    assert.equal(lastBody(f).get('pass'), 'synthetic-hidden-password');
    assert.equal(lastBody(f).get('open'), '0');
    f.nodes.open.checked = true; f.nodes.open.handlers.change(); await submit(f);
    assert.equal(lastBody(f).get('pass'), ''); assert.equal(lastBody(f).get('open'), '1');
    f.setScan(networks); await refresh(f);
    manual(f, false);
    assert.equal(f.nodes.manualNetwork.hidden, true); assert.equal(f.nodes.ssid.hidden, true);
    assert.equal(f.nodes.ssid.readOnly, true); assert.equal(f.nodes.ssid.required, false);
    assert.equal(f.nodes.networks.required, true); assert.equal(f.nodes.networks.disabled, false);
    assert.equal(f.nodes.networks.value, ''); assert.equal(f.nodes.ssid.value, '');
    assert.equal(f.nodes.pass.value, ''); assert.equal(f.nodes.open.checked, false);
    await submit(f); assert.equal(count(f, '/save'), 2);
    select(f, 'Fixture WiFi'); assert.equal(f.nodes.ssid.value, 'Fixture WiFi');
});

await test('scanning rejects parallel refresh/submit and idle poll cannot unlock controls', async () => {
    const hold = deferred(), f = fixture({scanReply:hold.promise}); await tick();
    assert.equal(count(f, '/scan'), 1); locked(f);
    assert.equal(f.nodes.ssid.value, ''); // saved SSID is not used before real results
    assert.match(f.nodes.scanStatus.textContent, /正在扫描/);
    await refresh(f); await submit(f); await poll(f);
    assert.equal(count(f, '/scan'), 1); assert.equal(count(f, '/save'), 0); locked(f);
    assert.equal(f.nodes.status.textContent, 'ready');
    f.enqueue('/status', new Error('synthetic poll failure')); await poll(f); locked(f);
    assert.match(f.nodes.scanStatus.textContent, /正在扫描/);
    assert.equal(f.nodes.status.textContent, 'ready');
    hold.resolve(networks); await tick();
    assert.equal(f.nodes.save.disabled, false); assert.equal(f.nodes.ssid.value, 'Fixture WiFi');
    const next = deferred(); f.setScan(next.promise);
    const scanning = refresh(f); await tick(); locked(f);
    await refresh(f); await submit(f); await poll(f); locked(f);
    assert.equal(count(f, '/scan'), 2); assert.equal(count(f, '/save'), 0);
    next.resolve(networks); await scanning;
    assert.equal(f.nodes.save.disabled, false);
});

await test('scan completion cannot unlock server-busy or completed state', async () => {
    for (const done of [false, true]) {
        const hold = deferred(), f = fixture({scanReply:hold.promise}); await tick();
        f.state.busy = true; f.state.done = done; f.state.stage = done ? 8 : 2;
        await poll(f); locked(f);
        hold.resolve(networks); await tick(); locked(f);
        await refresh(f); await submit(f);
        assert.equal(count(f, '/scan'), 1); assert.equal(count(f, '/save'), 0);
        if (done) assert.equal(f.nodes.status.dataset.tone, 'ok');
        else {
            f.state.busy = false; await poll(f);
            assert.equal(f.nodes.save.disabled, false);
        }
    }
});

await test('PAT/token/save/retry/success regression and form-error isolation', async () => {
    const f = fixture(); await tick(); select(f, 'Fixture Open', 0);
    f.nodes.pat.value = '  pt-synthetic-fixture-only  ';
    f.setError('bad input'); await submit(f);
    assert.equal(f.nodes.status.textContent, 'bad input'); assert.equal(f.nodes.save.disabled, false);
    await poll(f); assert.equal(f.nodes.status.textContent, 'bad input');
    await refresh(f); assert.equal(f.nodes.status.textContent, 'bad input');
    f.state.token = 'synthetic-refreshed-token'; await poll(f);
    f.nodes.ssid.value = 'synthetic-tampered-mirror'; f.nodes.pass.value = 'synthetic-unused-password';
    f.setError(null); await submit(f);
    assert.equal(f.nodes.pass.value, ''); assert.equal(f.nodes.pat.value, '');
    assert.equal(f.nodes.save.disabled, true);
    const request = f.requests.findLast(r => r.url === '/save');
    assert.equal(request.options.method, 'POST'); assert.equal(request.options.cache, 'no-store');
    assert.equal(request.options.headers['X-Portal-Token'], 'synthetic-refreshed-token');
    assert.equal(request.options.headers['Content-Type'], 'application/x-www-form-urlencoded');
    assert.equal(lastBody(f).get('ssid'), 'Fixture Open'); assert.equal(lastBody(f).get('pass'), '');
    assert.equal(lastBody(f).get('open'), '1');
    assert.equal(lastBody(f).get('pat'), 'pt-synthetic-fixture-only');
    assert.deepEqual([...lastBody(f).keys()].sort(), ['open','pass','pat','ssid']);
    f.state.stage = 5; f.state.message = 'PAT rejected'; await poll(f);
    assert.equal(f.nodes.status.dataset.tone, 'error'); assert.equal(f.nodes.save.disabled, false);
    f.nodes.pass.value = 'synthetic-sensitive'; f.nodes.pat.value = 'pt-synthetic-sensitive';
    f.state.stage = 8; f.state.busy = true; f.state.done = true; f.state.message = 'saved';
    await poll(f);
    assert.equal(f.nodes.status.dataset.tone, 'ok'); locked(f);
    assert.equal(f.nodes.pass.value, ''); assert.equal(f.nodes.pat.value, '');
    await submit(f); assert.equal(count(f, '/save'), 2);
});

await test('retained PAT, blank saved password, password toggles and missing token', async () => {
    const f = fixture({patSet:true}); await tick();
    assert.equal(f.nodes.pat.required, false); assert.equal(f.nodes.pat.value, '');
    assert.match(f.nodes.patHint.textContent, /留空保持/); assert.match(f.nodes.pat.placeholder, /留空保持/);
    for (const b of f.toggles) {
        const node = f.nodes[b.dataset.toggle];
        b.handlers.click(); assert.equal(node.type, 'text'); assert.equal(b.textContent, '隐藏');
        b.handlers.click(); assert.equal(node.type, 'password'); assert.equal(b.textContent, '显示');
    }
    f.state.token = ''; await poll(f); await submit(f);
    assert.equal(count(f, '/save'), 0); assert.match(f.nodes.status.textContent, /设备尚未就绪/);
    f.state.token = 'synthetic-restored-token'; await poll(f); await submit(f);
    assert.equal(lastBody(f).get('ssid'), 'Fixture WiFi');
    assert.equal(lastBody(f).get('pass'), ''); assert.equal(lastBody(f).get('pat'), '');
    assert.equal(lastBody(f).get('open'), '0');
});

await test('in-flight save resists polling unlock and parallel scan/submit; failures retain drafts', async () => {
    const f = fixture(); await tick();
    f.nodes.pass.value = 'synthetic-save-password'; f.nodes.pat.value = 'pt-synthetic-save';
    const hold = deferred(); f.enqueue('/save', hold.promise);
    const saving = submit(f); await tick(); locked(f);
    await poll(f); locked(f); await refresh(f); await submit(f);
    assert.equal(count(f, '/scan'), 1); assert.equal(count(f, '/save'), 1);
    hold.resolve({ok:false, body:{message:'synthetic save failure'}}); await saving;
    assert.equal(f.nodes.status.textContent, 'synthetic save failure');
    assert.equal(f.nodes.save.disabled, false);
    assert.equal(f.nodes.pass.value, 'synthetic-save-password');
    assert.equal(f.nodes.pat.value, 'pt-synthetic-save');
    const pending = deferred(); f.enqueue('/save', pending.promise);
    const retry = submit(f); await tick();
    f.state.busy = true; await poll(f); pending.resolve(new Error('synthetic transport failure'));
    await retry; locked(f);
});

await test('multibyte field limits remain device-validated, not maxlength-based byte validation', async () => {
    const f = fixture({patSet:true}); await tick(); manual(f);
    f.nodes.ssid.value = '测'.repeat(20); // 20 characters, 60 UTF-8 bytes: synthetic only
    f.nodes.pass.value = '密'.repeat(24); // 24 characters, 72 UTF-8 bytes
    assert.equal(f.nodes.ssid.attributes.maxlength, '32');
    assert.equal(f.nodes.pass.attributes.maxlength, '64');
    f.setError('synthetic byte-length validation failure'); await submit(f);
    assert.equal(lastBody(f).get('ssid'), '测'.repeat(20));
    assert.equal(lastBody(f).get('pass'), '密'.repeat(24));
    assert.equal(f.nodes.status.textContent, 'synthetic byte-length validation failure');
    assert.equal(f.nodes.save.disabled, false);
});

await test('PAT documentation, viewport and device-only factory reset instructions', async () => {
    assert.match(html, /https:\/\/docs\.qoder\.com\/zh\/cloud-agents\/api\/conventions\/authentication/);
    assert.match(html, /name="viewport"/);
    assert.ok(html.includes('恢复出厂设置：在设备的模板选择页或热点配置页长按 OK → 设备设置 → 恢复出厂设置。会删除 Wi-Fi、PAT 和音量设置，不删除云端会话；需要设备上二次确认。'));
    assert.doesNotMatch(html, /<button\b[^>]*>[^<]*(?:恢复出厂|重置)/);
});

console.log(`portal page mock tests: PASS (${passed} groups; synthetic DOM/HTTP only)`);
