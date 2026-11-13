/*
 * browser_jsrt.js - the JavaScript side of the Web Browser's script support.
 *
 * browser_js.c evaluates this once for every page that has scripts, then calls the
 * function it evaluates to with N, the page's natives (DOM nodes, timers, requests,
 * storage, dialogs - see browser_js.c), and the global object. It builds the web
 * platform on top of them: the DOM interfaces and their prototypes, events and their
 * dispatch, window / document / location / history / navigator, fetch,
 * XMLHttpRequest, storage, observers and the small things pages expect.
 * It returns the hooks the browser calls (dispatch an event, make an event, ...).
 */
(function (N, G) {
'use strict';

const LS = Symbol('listeners'), HS = Symbol('handlers'), HSRC = Symbol('handlerSource');
const ES = Symbol('eventState');
const hidden = (o, k, v) => Object.defineProperty(o, k, { value: v, writable: true, configurable: true });
const def = (o, props) => {
    for (const k of Reflect.ownKeys(props)) {
        const d = Object.getOwnPropertyDescriptor(props, k);
        if ('value' in d) d.enumerable = false;
        Object.defineProperty(o, k, d);
    }
};
const str = (v) => (v === null ? 'null' : String(v));
const kebab = (s) => s.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
const camel = (s) => s.replace(/-([a-z])/g, (m, c) => c.toUpperCase());

/* ---- DOMException ------------------------------------------------------------------------ */
const EXC_CODES = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4, InvalidCharacterError: 5,
    NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11, SyntaxError: 12, InvalidAccessError: 15,
    TypeMismatchError: 17, SecurityError: 18, NetworkError: 19, AbortError: 20, QuotaExceededError: 22,
    TimeoutError: 23, DataCloneError: 25 };
class DOMException extends Error {
    constructor(message = '', name = 'Error') {
        super(message);
        hidden(this, 'name', String(name));
        hidden(this, 'code', EXC_CODES[name] || 0);
    }
}
hidden(G, 'DOMException', DOMException);

/* ---- interfaces -------------------------------------------------------------------------- */
function iface(name, parent, ctor) {
    const C = ctor || function () { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(C, 'name', { value: name, configurable: true });
    if (parent) { Object.setPrototypeOf(C.prototype, parent.prototype); Object.setPrototypeOf(C, parent); }
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
    hidden(G, name, C);
    return C;
}

class EventTarget {}
hidden(G, 'EventTarget', EventTarget);
Object.defineProperty(EventTarget.prototype, Symbol.toStringTag, { value: 'EventTarget', configurable: true });
const Window = iface('Window', EventTarget);
const Node = iface('Node', EventTarget);
const Element = iface('Element', Node);
const HTMLElement = iface('HTMLElement', Element);
const SVGElement = iface('SVGElement', Element);
const CharacterData = iface('CharacterData', Node);
const Text = iface('Text', CharacterData, function (data = '') { return N.create(3, String(data)); });
const Comment = iface('Comment', CharacterData, function (data = '') { return N.create(8, String(data)); });
const DocumentFragment = iface('DocumentFragment', Node, function () { return N.create(11, ''); });
const Document = iface('Document', Node);
const HTMLDocument = iface('HTMLDocument', Document);
const ShadowRoot = iface('ShadowRoot', DocumentFragment);
Text.prototype.constructor = Text;
const TAGS = {
    a: 'Anchor', area: 'Area', audio: 'Audio', br: 'BR', base: 'Base', body: 'Body', button: 'Button',
    canvas: 'Canvas', dl: 'DList', data: 'Data', datalist: 'DataList', details: 'Details', dialog: 'Dialog',
    div: 'Div', embed: 'Embed', fieldset: 'FieldSet', form: 'Form', h1: 'Heading', h2: 'Heading', h3: 'Heading',
    h4: 'Heading', h5: 'Heading', h6: 'Heading', hr: 'HR', head: 'Head', html: 'Html', iframe: 'IFrame',
    img: 'Image', input: 'Input', li: 'LI', label: 'Label', legend: 'Legend', link: 'Link', map: 'Map',
    menu: 'Menu', meta: 'Meta', meter: 'Meter', ins: 'Mod', del: 'Mod', ol: 'OList', object: 'Object',
    optgroup: 'OptGroup', option: 'Option', output: 'Output', p: 'Paragraph', param: 'Param', picture: 'Picture',
    pre: 'Pre', progress: 'Progress', q: 'Quote', blockquote: 'Quote', script: 'Script', select: 'Select',
    slot: 'Slot', source: 'Source', span: 'Span', style: 'Style', caption: 'TableCaption', td: 'TableCell',
    th: 'TableCell', col: 'TableCol', colgroup: 'TableCol', table: 'Table', tr: 'TableRow', thead: 'TableSection',
    tbody: 'TableSection', tfoot: 'TableSection', template: 'Template', textarea: 'TextArea', time: 'Time',
    title: 'Title', track: 'Track', ul: 'UList', video: 'Video',
};
const MediaElement = iface('HTMLMediaElement', HTMLElement);
const tagProtos = {}, ifaces = {};
for (const t of Object.keys(TAGS)) {
    const nm = 'HTML' + TAGS[t] + 'Element';
    if (!ifaces[nm]) ifaces[nm] = iface(nm, nm === 'HTMLAudioElement' || nm === 'HTMLVideoElement' ? MediaElement : HTMLElement);
    tagProtos[t] = ifaces[nm].prototype;
}
const HTMLUnknownElement = iface('HTMLUnknownElement', HTMLElement);
const I = (n) => ifaces['HTML' + n + 'Element'];

const document = N.init({
    EventTarget, Window, Node, Element, HTMLElement, SVGElement, CharacterData, Text, Comment,
    DocumentFragment, Document: HTMLDocument, HTMLUnknownElement, tags: tagProtos,
});
hidden(G, 'document', document);

/* ---- events -------------------------------------------------------------------------------- */
const PH_NONE = 0, PH_CAPTURE = 1, PH_TARGET = 2, PH_BUBBLE = 3;
class Event {
    constructor(type, init) {
        if (arguments.length < 1) throw new TypeError("Failed to construct 'Event': 1 argument required");
        init = init || {};
        hidden(this, ES, { stop: false, imm: false, passive: false, dispatching: false, path: [] });
        this.type = String(type);
        this.bubbles = !!init.bubbles;
        this.cancelable = !!init.cancelable;
        this.composed = !!init.composed;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.eventPhase = PH_NONE;
        this.isTrusted = false;
        this.timeStamp = N.now();
    }
    get srcElement() { return this.target; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this[ES].stop; }
    set cancelBubble(v) { if (v) this[ES].stop = true; }
    preventDefault() { if (this.cancelable && !this[ES].passive) this.defaultPrevented = true; }
    stopPropagation() { this[ES].stop = true; }
    stopImmediatePropagation() { this[ES].stop = this[ES].imm = true; }
    composedPath() { return this[ES].dispatching ? this[ES].path.slice() : []; }
    initEvent(type, bubbles, cancelable) {
        this.type = String(type); this.bubbles = !!bubbles; this.cancelable = !!cancelable;
        this.defaultPrevented = false;
    }
}
Object.assign(Event, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });
Object.assign(Event.prototype, { NONE: 0, CAPTURING_PHASE: 1, AT_TARGET: 2, BUBBLING_PHASE: 3 });
class UIEvent extends Event {
    constructor(type, init) { super(type, init); init = init || {}; this.view = init.view || null; this.detail = init.detail || 0; }
    get which() { return 0; }
}
const MODS = (o, i) => {
    o.ctrlKey = !!i.ctrlKey; o.shiftKey = !!i.shiftKey; o.altKey = !!i.altKey; o.metaKey = !!i.metaKey;
};
const getMod = function (k) { return k === 'Control' ? this.ctrlKey : k === 'Shift' ? this.shiftKey : k === 'Alt' ? this.altKey : k === 'Meta' ? this.metaKey : false; };
class MouseEvent extends UIEvent {
    constructor(type, init) {
        super(type, init); const i = init || {};
        this.screenX = i.screenX || 0; this.screenY = i.screenY || 0;
        this.clientX = i.clientX || 0; this.clientY = i.clientY || 0;
        this.button = i.button || 0; this.buttons = i.buttons || 0;
        this.relatedTarget = i.relatedTarget || null;
        MODS(this, i);
    }
    get pageX() { return this.clientX + G.scrollX; }
    get pageY() { return this.clientY + G.scrollY; }
    get x() { return this.clientX; }
    get y() { return this.clientY; }
    get offsetX() { const r = this.target && this.target.getBoundingClientRect ? this.target.getBoundingClientRect() : null; return r ? this.clientX - r.left : this.clientX; }
    get offsetY() { const r = this.target && this.target.getBoundingClientRect ? this.target.getBoundingClientRect() : null; return r ? this.clientY - r.top : this.clientY; }
    get movementX() { return 0; }
    get movementY() { return 0; }
    get which() { return this.button + 1; }
    getModifierState(k) { return getMod.call(this, k); }
}
class PointerEvent extends MouseEvent {
    constructor(type, init) {
        super(type, init); const i = init || {};
        this.pointerId = i.pointerId || 1; this.pointerType = i.pointerType || 'mouse'; this.isPrimary = true;
        this.width = 1; this.height = 1; this.pressure = i.pressure || 0; this.tiltX = 0; this.tiltY = 0;
    }
}
class WheelEvent extends MouseEvent {
    constructor(type, init) { super(type, init); const i = init || {}; this.deltaX = i.deltaX || 0; this.deltaY = i.deltaY || 0; this.deltaZ = 0; this.deltaMode = 0; }
}
class KeyboardEvent extends UIEvent {
    constructor(type, init) {
        super(type, init); const i = init || {};
        this.key = i.key || ''; this.code = i.code || ''; this.location = i.location || 0;
        this.repeat = !!i.repeat; this.isComposing = false;
        this.keyCode = i.keyCode || 0; this.charCode = i.charCode || 0;
        MODS(this, i);
    }
    get which() { return this.keyCode || this.charCode; }
    getModifierState(k) { return getMod.call(this, k); }
}
class FocusEvent extends UIEvent {
    constructor(type, init) { super(type, init); this.relatedTarget = (init && init.relatedTarget) || null; }
}
class InputEvent extends UIEvent {
    constructor(type, init) { super(type, init); const i = init || {}; this.data = i.data === undefined ? null : i.data; this.inputType = i.inputType || ''; this.isComposing = false; }
}
class CompositionEvent extends UIEvent {}
class CustomEvent extends Event {
    constructor(type, init) { super(type, init); this.detail = init && init.detail !== undefined ? init.detail : null; }
    initCustomEvent(type, b, c, detail) { this.initEvent(type, b, c); this.detail = detail; }
}
class SubmitEvent extends Event { constructor(t, i) { super(t, i); this.submitter = (i && i.submitter) || null; } }
class HashChangeEvent extends Event { constructor(t, i) { super(t, i); this.oldURL = (i && i.oldURL) || ''; this.newURL = (i && i.newURL) || ''; } }
class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && i.state !== undefined ? i.state : null; } }
class PageTransitionEvent extends Event { constructor(t, i) { super(t, i); this.persisted = !!(i && i.persisted); } }
class ProgressEvent extends Event {
    constructor(t, i) { super(t, i); i = i || {}; this.lengthComputable = !!i.lengthComputable; this.loaded = i.loaded || 0; this.total = i.total || 0; }
}
class ErrorEvent extends Event {
    constructor(t, i) { super(t, i); i = i || {}; this.message = i.message || ''; this.filename = i.filename || ''; this.lineno = i.lineno || 0; this.colno = i.colno || 0; this.error = i.error; }
}
class MessageEvent extends Event {
    constructor(t, i) { super(t, i); i = i || {}; this.data = i.data === undefined ? null : i.data; this.origin = i.origin || ''; this.lastEventId = ''; this.source = i.source || null; this.ports = i.ports || []; }
}
class StorageEvent extends Event {
    constructor(t, i) { super(t, i); i = i || {}; this.key = i.key ?? null; this.oldValue = i.oldValue ?? null; this.newValue = i.newValue ?? null; this.url = i.url || ''; this.storageArea = i.storageArea || null; }
}
class AnimationEvent extends Event {}
class TransitionEvent extends Event {}
class ClipboardEvent extends Event { constructor(t, i) { super(t, i); this.clipboardData = null; } }
class DragEvent extends MouseEvent { constructor(t, i) { super(t, i); this.dataTransfer = null; } }
class TouchEvent extends UIEvent { constructor(t, i) { super(t, i); this.touches = []; this.targetTouches = []; this.changedTouches = []; } }
for (const C of [Event, UIEvent, MouseEvent, PointerEvent, WheelEvent, KeyboardEvent, FocusEvent, InputEvent,
    CompositionEvent, CustomEvent, SubmitEvent, HashChangeEvent, PopStateEvent, PageTransitionEvent, ProgressEvent,
    ErrorEvent, MessageEvent, StorageEvent, AnimationEvent, TransitionEvent, ClipboardEvent, DragEvent, TouchEvent]) {
    hidden(G, C.name, C);
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: C.name, configurable: true });
}
delete G.TouchEvent;     /* (no touch screen: pages test 'ontouchstart' in window / TouchEvent) */

function report(e) {
    let msg;
    try {
        msg = e instanceof Error ? (e.name + ': ' + e.message + (e.stack ? '\n' + e.stack : '')) : 'Uncaught ' + fmt(e);
    } catch (x) { msg = 'Uncaught exception'; }
    N.log(3, msg.indexOf('Uncaught') === 0 ? msg : 'Uncaught ' + msg);
    try {
        const ev = new ErrorEvent('error', { message: e && e.message ? String(e.message) : String(e), error: e, cancelable: true });
        invoke(G, ev, PH_TARGET);
    } catch (x) { /* ignore */ }
}
hidden(G, 'reportError', report);

function listeners(t, type, make) {
    let m = t[LS];
    if (!m) { if (!make) return null; m = new Map(); hidden(t, LS, m); }
    let a = m.get(type);
    if (!a && make) m.set(type, (a = []));
    return a;
}
function flatOpts(o) {
    if (typeof o === 'boolean') return { capture: o, once: false, passive: false, signal: null };
    o = o || {};
    return { capture: !!o.capture, once: !!o.once, passive: !!o.passive, signal: o.signal || null };
}
def(EventTarget.prototype, {
    addEventListener(type, cb, opts) {
        if (cb === null || cb === undefined) return;
        type = String(type);
        const o = flatOpts(opts);
        if (o.signal && o.signal.aborted) return;
        const t = this === undefined || this === null ? G : this;
        const a = listeners(t, type, true);
        for (const l of a) if (l.cb === cb && l.capture === o.capture && !l.handler) return;
        a.push({ cb, capture: o.capture, once: o.once, passive: o.passive, removed: false });
        N.listen(type);
        if (o.signal) o.signal.addEventListener('abort', () => t.removeEventListener(type, cb, o.capture));
    },
    removeEventListener(type, cb, opts) {
        const t = this === undefined || this === null ? G : this;
        const a = listeners(t, String(type), false);
        if (!a) return;
        const capture = flatOpts(opts).capture;
        for (let i = 0; i < a.length; i++)
            if (a[i].cb === cb && a[i].capture === capture && !a[i].handler) { a[i].removed = true; a.splice(i, 1); return; }
    },
    dispatchEvent(ev) {
        if (!(ev instanceof Event)) throw new TypeError("Failed to execute 'dispatchEvent': parameter 1 is not of type 'Event'.");
        if (ev[ES].dispatching) throw new DOMException('The event is already being dispatched.', 'InvalidStateError');
        ev.isTrusted = false;
        dispatch(this === undefined || this === null ? G : this, ev);
        return !ev.defaultPrevented;
    },
});

/* inline handlers: onclick="..." attributes and the onclick properties */
const EVENT_NAMES = ('abort,afterprint,animationend,animationiteration,animationstart,auxclick,beforeinput,beforeprint,' +
    'beforeunload,blur,cancel,canplay,canplaythrough,change,click,close,contextmenu,copy,cut,dblclick,drag,dragend,' +
    'dragenter,dragleave,dragover,dragstart,drop,durationchange,emptied,ended,error,focus,focusin,focusout,hashchange,' +
    'input,invalid,keydown,keypress,keyup,languagechange,load,loadeddata,loadedmetadata,loadend,loadstart,message,' +
    'messageerror,mousedown,mouseenter,mouseleave,mousemove,mouseout,mouseover,mouseup,offline,online,pagehide,' +
    'pageshow,paste,pause,play,playing,pointercancel,pointerdown,pointerenter,pointerleave,pointermove,pointerout,' +
    'pointerover,pointerup,popstate,progress,ratechange,reset,resize,scroll,scrollend,search,seeked,seeking,select,' +
    'selectionchange,selectstart,stalled,storage,submit,suspend,timeupdate,toggle,transitionend,unhandledrejection,' +
    'unload,volumechange,waiting,wheel').split(',');
const WINDOW_BODY_EVENTS = new Set(['load', 'unload', 'beforeunload', 'resize', 'hashchange', 'popstate', 'pageshow',
    'pagehide', 'online', 'offline', 'message', 'storage', 'afterprint', 'beforeprint']);
function handlerMap(t) { let m = t[HS]; if (!m) { m = new Map(); hidden(t, HS, m); } return m; }
function setHandler(t, type, fn) {
    const m = handlerMap(t);
    const a = listeners(t, type, true);
    const had = a.some((l) => l.handler);
    m.set(type, typeof fn === 'function' || (fn && typeof fn === 'object') ? fn : null);
    if (!had) a.push({ handler: true, capture: false, once: false, passive: false, removed: false });
    N.listen(type);
}
function compileHandler(code, el) {
    try {
        return new Function('event', code);
    } catch (e) { report(e); return null; }
}
/* the handler of an element's on<type> attribute (made when first needed, again when it changed) */
function attrHandler(t, type) {
    let src = null, host = t;
    if (t === G) { host = document.body; if (!host || !WINDOW_BODY_EVENTS.has(type)) return; }
    else if (!(t instanceof Element)) return;
    else if (WINDOW_BODY_EVENTS.has(type) && (t.localName === 'body' || t.localName === 'frameset')) return;
    src = N.attr(host, 'on' + type);
    const srcs = t[HSRC] || (hidden(t, HSRC, new Map()), t[HSRC]);
    if (src === null) { if (srcs.has(type)) { srcs.delete(type); handlerMap(t).set(type, null); } return; }
    if (srcs.get(type) === src) return;
    srcs.set(type, src);
    const fn = compileHandler(src, t);
    const a = listeners(t, type, true);
    if (!a.some((l) => l.handler)) a.unshift({ handler: true, capture: false, once: false, passive: false, removed: false });
    handlerMap(t).set(type, fn);
}
function handlerProps(proto, isWindow) {
    for (const nm of EVENT_NAMES) {
        Object.defineProperty(proto, 'on' + nm, {
            get() {
                const t = this === undefined || this === null ? G : this;
                attrHandler(t, nm);
                const m = t[HS];
                return (m && m.get(nm)) || null;
            },
            set(fn) {
                const t = this === undefined || this === null ? G : this;
                if (!isWindow && t instanceof Element && (t.localName === 'body' || t.localName === 'frameset') && WINDOW_BODY_EVENTS.has(nm)) { G['on' + nm] = fn; return; }
                const srcs = t[HSRC] || (hidden(t, HSRC, new Map()), t[HSRC]);
                srcs.set(nm, t === G ? N.attr(document.body || document.documentElement, 'on' + nm) : t instanceof Element ? N.attr(t, 'on' + nm) : null);
                setHandler(t, nm, fn);
            },
            configurable: true, enumerable: true,
        });
    }
}
handlerProps(HTMLElement.prototype, false);
handlerProps(SVGElement.prototype, false);
handlerProps(Document.prototype, false);
handlerProps(Window.prototype, true);

function invoke(node, ev, phase) {
    if (node instanceof Element || node === G) attrHandler(node, ev.type);
    const a = node[LS] && node[LS].get(ev.type);
    if (!a || !a.length) return;
    ev.currentTarget = node;
    const st = ev[ES];
    for (const l of a.slice()) {
        if (l.removed) continue;
        if (phase === PH_CAPTURE && !l.capture) continue;
        if (phase === PH_BUBBLE && l.capture) continue;
        if (l.once) { l.removed = true; const i = a.indexOf(l); if (i >= 0) a.splice(i, 1); }
        st.passive = l.passive;
        try {
            if (l.handler) {
                const fn = node[HS] && node[HS].get(ev.type);
                if (typeof fn === 'function') {
                    let r;
                    if (ev.type === 'error' && node === G && ev instanceof ErrorEvent) r = fn.call(node, ev.message, ev.filename, ev.lineno, ev.colno, ev.error);
                    else r = fn.call(node, ev);
                    if (ev.type === 'beforeunload' ? (r !== undefined && r !== null) : r === false) ev.preventDefault();
                    if (ev.type === 'error' && node === G && r === true) ev.preventDefault();
                }
            } else if (typeof l.cb === 'function') {
                l.cb.call(node, ev);
            } else if (l.cb && typeof l.cb.handleEvent === 'function') {
                l.cb.handleEvent(ev);
            }
        } catch (e) {
            if (ev.type === 'error' && node === G) N.log(3, 'Uncaught ' + fmt(e));
            else report(e);
        }
        st.passive = false;
        if (st.imm) break;
    }
}

function eventPath(target, ev) {
    const path = [];
    for (let n = target; n; n = n.parentNode) path.push(n);
    if (path.length && path[path.length - 1] === document && ev.type !== 'load') path.push(G);
    return path;
}
function dispatch(target, ev) {
    const st = ev[ES];
    st.dispatching = true;
    st.stop = st.imm = false;
    const path = eventPath(target, ev);
    st.path = path;
    ev.target = target;
    try {
        for (let i = path.length - 1; i > 0 && !st.stop; i--) { ev.eventPhase = PH_CAPTURE; invoke(path[i], ev, PH_CAPTURE); }
        if (!st.stop) { ev.eventPhase = PH_TARGET; invoke(target, ev, PH_TARGET); }
        if (ev.bubbles) for (let i = 1; i < path.length && !st.stop; i++) { ev.eventPhase = PH_BUBBLE; invoke(path[i], ev, PH_BUBBLE); }
    } finally {
        ev.eventPhase = PH_NONE;
        ev.currentTarget = null;
        st.dispatching = false;
    }
    return !ev.defaultPrevented;
}

/* ---- formatting for console ------------------------------------------------------------- */
function fmt(v, depth = 0, seen = []) {
    const t = typeof v;
    if (t === 'string') return depth ? JSON.stringify(v) : v;
    if (t === 'number' || t === 'boolean' || t === 'undefined' || v === null || t === 'bigint') return String(v);
    if (t === 'symbol') return v.toString();
    if (t === 'function') return '[Function: ' + (v.name || 'anonymous') + ']';
    if (v instanceof Error) return v.name + ': ' + v.message;
    if (v instanceof Node) {
        if (v.nodeType === 1) return '<' + v.localName + (v.id ? '#' + v.id : '') + (v.className && typeof v.className === 'string' ? '.' + v.className.trim().split(/\s+/).join('.') : '') + '>';
        if (v.nodeType === 3) return '#text ' + JSON.stringify(v.data.slice(0, 60));
        return v.nodeName;
    }
    if (seen.includes(v)) return '[Circular]';
    if (depth > 2) return Array.isArray(v) ? '[Array]' : '[Object]';
    seen.push(v);
    try {
        if (Array.isArray(v)) {
            const parts = v.slice(0, 50).map((x) => fmt(x, depth + 1, seen));
            if (v.length > 50) parts.push('... ' + (v.length - 50) + ' more');
            return '[' + parts.join(', ') + ']';
        }
        if (v instanceof Map) return 'Map(' + v.size + ')';
        if (v instanceof Set) return 'Set(' + v.size + ')';
        if (v instanceof Promise) return 'Promise';
        if (v instanceof Date) return v.toISOString();
        if (v instanceof RegExp) return String(v);
        const keys = Object.keys(v);
        const parts = keys.slice(0, 30).map((k) => k + ': ' + fmt(v[k], depth + 1, seen));
        if (keys.length > 30) parts.push('...');
        const tag = v.constructor && v.constructor.name && v.constructor.name !== 'Object' ? v.constructor.name + ' ' : '';
        return tag + '{' + parts.join(', ') + '}';
    } catch (e) {
        return '[Object]';
    } finally {
        seen.pop();
    }
}
function fmtArgs(args) {
    let out = '', i = 0;
    if (typeof args[0] === 'string' && args[0].indexOf('%') >= 0 && args.length > 1) {
        i = 1;
        out = args[0].replace(/%[sdifoOc%]/g, (m) => {
            if (m === '%%') return '%';
            if (i >= args.length) return m;
            const a = args[i++];
            if (m === '%c') return '';
            if (m === '%d' || m === '%i') return String(parseInt(a, 10));
            if (m === '%f') return String(Number(a));
            if (m === '%s') return typeof a === 'string' ? a : fmt(a, 1);
            return fmt(a, 1);
        });
    } else if (args.length) {
        out = fmt(args[0]);
        i = 1;
    }
    for (; i < args.length; i++) out += ' ' + fmt(args[i]);
    return out;
}
const counts = new Map(), timers = new Map();
let groupDepth = 0;
const clog = (lvl) => function (...a) { N.log(lvl, '  '.repeat(groupDepth) + fmtArgs(a)); };
const console_ = {
    log: clog(0), info: clog(1), warn: clog(2), error: clog(3), debug: clog(4), trace: clog(4), dir: clog(0),
    dirxml: clog(0), table: clog(0),
    assert(c, ...a) { if (!c) N.log(3, 'Assertion failed' + (a.length ? ': ' + fmtArgs(a) : '')); },
    count(l = 'default') { counts.set(l, (counts.get(l) || 0) + 1); N.log(0, l + ': ' + counts.get(l)); },
    countReset(l = 'default') { counts.delete(l); },
    time(l = 'default') { timers.set(l, N.now()); },
    timeLog(l = 'default', ...a) { if (timers.has(l)) N.log(0, l + ': ' + (N.now() - timers.get(l)).toFixed(1) + ' ms ' + fmtArgs(a)); },
    timeEnd(l = 'default') { if (timers.has(l)) { N.log(0, l + ': ' + (N.now() - timers.get(l)).toFixed(1) + ' ms'); timers.delete(l); } },
    group(...a) { if (a.length) N.log(0, fmtArgs(a)); groupDepth++; },
    groupCollapsed(...a) { if (a.length) N.log(0, fmtArgs(a)); groupDepth++; },
    groupEnd() { if (groupDepth) groupDepth--; },
    clear() {},
    profile() {}, profileEnd() {}, timeStamp() {},
};
hidden(G, 'console', console_);

/* ---- node lists ----------------------------------------------------------------------------- */
const NodeList = iface('NodeList', null);
def(NodeList.prototype, {
    item(i) { return this[i] === undefined ? null : this[i]; },
    forEach(f, t) { for (let i = 0; i < this.length; i++) f.call(t, this[i], i, this); },
    entries() { return Array.prototype.entries.call(this); },
    keys() { return Array.prototype.keys.call(this); },
    values() { return Array.prototype.values.call(this); },
    [Symbol.iterator]() { return Array.prototype.values.call(this); },
});
const HTMLCollection = iface('HTMLCollection', null);
def(HTMLCollection.prototype, {
    item(i) { return this[i] === undefined ? null : this[i]; },
    namedItem(n) { for (let i = 0; i < this.length; i++) { const e = this[i]; if (e.id === n || e.getAttribute('name') === n) return e; } return null; },
    [Symbol.iterator]() { return Array.prototype.values.call(this); },
});
const HTMLOptionsCollection = iface('HTMLOptionsCollection', HTMLCollection);
/* a static list: an array that is a NodeList */
function staticList(a) {
    const l = Object.create(NodeList.prototype);
    for (let i = 0; i < a.length; i++) l[i] = a[i];
    Object.defineProperty(l, 'length', { value: a.length });
    return l;
}
/* live lists: recomputed when the document changed (N.version) */
function liveList(compute, proto) {
    let ver = -1, cur = [];
    const get = () => { const v = N.version(); if (v !== ver) { cur = compute(); ver = v; } return cur; };
    const idx = (k) => (typeof k === 'string' && /^(0|[1-9][0-9]*)$/.test(k) ? +k : -1);
    return new Proxy(Object.create(proto || NodeList.prototype), {
        get(t, k, r) {
            if (k === 'length') return get().length;
            const i = idx(k);
            if (i >= 0) return get()[i];
            if (k === Symbol.iterator) return function () { return get().slice()[Symbol.iterator](); };
            if (k in t) return Reflect.get(t, k, r);
            if (typeof k === 'string' && proto === HTMLCollection.prototype) {
                const e = get().find((x) => x.id === k || (x.getAttribute && x.getAttribute('name') === k));
                if (e) return e;
            }
            return undefined;
        },
        has(t, k) { const i = idx(k); return i >= 0 ? i < get().length : k === 'length' || k in t; },
        ownKeys() { return get().map((x, i) => String(i)).concat(['length']); },
        getOwnPropertyDescriptor(t, k) {
            if (k === 'length') return { value: get().length, writable: false, enumerable: false, configurable: true };
            const i = idx(k);
            if (i >= 0 && i < get().length) return { value: get()[i], writable: false, enumerable: true, configurable: true };
            return undefined;
        },
        set() { return true; },
    });
}

/* ---- Node ----------------------------------------------------------------------------------- */
Object.assign(Node, { ELEMENT_NODE: 1, ATTRIBUTE_NODE: 2, TEXT_NODE: 3, CDATA_SECTION_NODE: 4, PROCESSING_INSTRUCTION_NODE: 7,
    COMMENT_NODE: 8, DOCUMENT_NODE: 9, DOCUMENT_TYPE_NODE: 10, DOCUMENT_FRAGMENT_NODE: 11,
    DOCUMENT_POSITION_DISCONNECTED: 1, DOCUMENT_POSITION_PRECEDING: 2, DOCUMENT_POSITION_FOLLOWING: 4,
    DOCUMENT_POSITION_CONTAINS: 8, DOCUMENT_POSITION_CONTAINED_BY: 16, DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC: 32 });
Object.assign(Node.prototype, Node);
const toNode = (x) => (x instanceof Node ? x : document.createTextNode(String(x)));
const nodesArg = (args) => {
    if (args.length === 1) return toNode(args[0]);
    const f = document.createDocumentFragment();
    for (const a of args) f.appendChild(toNode(a));
    return f;
};
def(Node.prototype, {
    get childNodes() { const n = this; return liveList(() => N.childNodes(n)); },
    get textContent() { return this.nodeType === 9 ? null : N.text(this); },
    set textContent(v) { if (this.nodeType !== 9) N.setText(this, v === null || v === undefined ? '' : String(v)); },
    get nodeValue() { const t = this.nodeType; return t === 3 || t === 8 ? N.data(this) : null; },
    set nodeValue(v) { const t = this.nodeType; if (t === 3 || t === 8) N.setData(this, v === null ? '' : String(v)); },
    get baseURI() { return document.baseURI; },
    appendChild(c) { if (!(c instanceof Node)) throw new TypeError("Failed to execute 'appendChild' on 'Node': parameter 1 is not of type 'Node'."); return N.insert(this, c, null); },
    insertBefore(c, ref) { if (!(c instanceof Node)) throw new TypeError("Failed to execute 'insertBefore' on 'Node': parameter 1 is not of type 'Node'."); return N.insert(this, c, ref === undefined ? null : ref); },
    removeChild(c) {
        if (!(c instanceof Node) || c.parentNode !== this) throw new DOMException("Failed to execute 'removeChild' on 'Node': The node to be removed is not a child of this node.", 'NotFoundError');
        return N.remove(c);
    },
    replaceChild(nc, old) {
        if (!(old instanceof Node) || old.parentNode !== this) throw new DOMException("Failed to execute 'replaceChild' on 'Node': The node to be replaced is not a child of this node.", 'NotFoundError');
        if (nc === old) return old;
        const next = old.nextSibling === nc ? nc.nextSibling : old.nextSibling;
        N.remove(old);
        N.insert(this, nc, next);
        return old;
    },
    cloneNode(deep) { return N.clone(this, !!deep); },
    hasChildNodes() { return this.firstChild !== null; },
    contains(o) { return o instanceof Node ? N.contains(this, o) : false; },
    getRootNode() { let n = this; while (n.parentNode) n = n.parentNode; return n; },
    isSameNode(o) { return this === o; },
    isEqualNode(o) {
        if (!(o instanceof Node) || o.nodeType !== this.nodeType || o.nodeName !== this.nodeName) return false;
        if (this.nodeType === 3 || this.nodeType === 8) return this.data === o.data;
        if (this.nodeType === 1) {
            const a = this.getAttributeNames(), b = o.getAttributeNames();
            if (a.length !== b.length || a.some((k) => this.getAttribute(k) !== o.getAttribute(k))) return false;
        }
        const x = this.childNodes, y = o.childNodes;
        if (x.length !== y.length) return false;
        for (let i = 0; i < x.length; i++) if (!x[i].isEqualNode(y[i])) return false;
        return true;
    },
    compareDocumentPosition(o) {
        if (o === this) return 0;
        if (!(o instanceof Node)) throw new TypeError('not a Node');
        if (this.getRootNode() !== o.getRootNode()) return 1 | 32 | 2;
        if (N.contains(this, o)) return 16 | 4;
        if (N.contains(o, this)) return 8 | 2;
        return N.order(this, o) < 0 ? 4 : 2;
    },
    normalize() {
        let c = this.firstChild;
        while (c) {
            if (c.nodeType === 3) {
                let n = c.nextSibling;
                while (n && n.nodeType === 3) { c.data += n.data; const nn = n.nextSibling; this.removeChild(n); n = nn; }
                const next = c.nextSibling;
                if (c.data === '') this.removeChild(c);
                c = next;
                continue;
            }
            if (c.nodeType === 1) c.normalize();
            c = c.nextSibling;
        }
    },
    lookupNamespaceURI() { return null; },
    isDefaultNamespace(ns) { return !ns; },
});
/* ChildNode / ParentNode */
const childNodeMixin = {
    remove() { if (this.parentNode) N.remove(this); },
    before(...a) { const p = this.parentNode; if (p) p.insertBefore(nodesArg(a), this); },
    after(...a) { const p = this.parentNode; if (p) p.insertBefore(nodesArg(a), this.nextSibling); },
    replaceWith(...a) { const p = this.parentNode; if (p) { const n = nodesArg(a); p.insertBefore(n, this); if (this.parentNode === p) p.removeChild(this); } },
};
const parentNodeMixin = {
    get children() { const n = this; return liveList(() => N.children(n), HTMLCollection.prototype); },
    get firstElementChild() { return N.children(this)[0] || null; },
    get lastElementChild() { const c = N.children(this); return c[c.length - 1] || null; },
    get childElementCount() { return N.children(this).length; },
    append(...a) { if (a.length) this.appendChild(nodesArg(a)); },
    prepend(...a) { if (a.length) this.insertBefore(nodesArg(a), this.firstChild); },
    replaceChildren(...a) { while (this.firstChild) N.remove(this.firstChild); if (a.length) this.appendChild(nodesArg(a)); },
    querySelector(s) { return N.query(this, String(s), false); },
    querySelectorAll(s) { return staticList(N.query(this, String(s), true)); },
    getElementsByTagName(t) { const n = this; t = String(t); return liveList(() => N.byTag(n, t), HTMLCollection.prototype); },
    getElementsByClassName(c) { const n = this; c = String(c); return liveList(() => N.byClass(n, c), HTMLCollection.prototype); },
};
def(Element.prototype, childNodeMixin);
def(CharacterData.prototype, childNodeMixin);
def(Element.prototype, parentNodeMixin);
def(Document.prototype, parentNodeMixin);
def(DocumentFragment.prototype, parentNodeMixin);
def(DocumentFragment.prototype, {
    getElementById(id) { return N.byId(this, String(id)); },
    get nodeName() { return '#document-fragment'; },
});

/* ---- CharacterData ------------------------------------------------------------------------- */
def(CharacterData.prototype, {
    get data() { return N.data(this); },
    set data(v) { N.setData(this, v === null ? '' : String(v)); },
    get length() { return N.data(this).length; },
    get previousElementSibling() { let n = this.previousSibling; while (n && n.nodeType !== 1) n = n.previousSibling; return n; },
    get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n = n.nextSibling; return n; },
    substringData(o, c) { return this.data.substr(o, c); },
    appendData(s) { this.data += String(s); },
    insertData(o, s) { const d = this.data; this.data = d.slice(0, o) + s + d.slice(o); },
    deleteData(o, c) { const d = this.data; this.data = d.slice(0, o) + d.slice(o + c); },
    replaceData(o, c, s) { const d = this.data; this.data = d.slice(0, o) + s + d.slice(o + c); },
});
def(Text.prototype, {
    get wholeText() { return this.data; },
    splitText(o) {
        const d = this.data;
        const t = document.createTextNode(d.slice(o));
        this.data = d.slice(0, o);
        if (this.parentNode) this.parentNode.insertBefore(t, this.nextSibling);
        return t;
    },
});

/* ---- Element -------------------------------------------------------------------------------- */
class DOMTokenList {
    constructor(el, attr) { hidden(this, '_e', el); hidden(this, '_a', attr); }
    _get() { const v = this._e.getAttribute(this._a); return v ? v.trim().split(/\s+/).filter(Boolean) : []; }
    _set(l) { this._e.setAttribute(this._a, l.join(' ')); }
    get length() { return this._get().length; }
    get value() { return this._e.getAttribute(this._a) || ''; }
    set value(v) { this._e.setAttribute(this._a, String(v)); }
    item(i) { const l = this._get(); return i >= 0 && i < l.length ? l[i] : null; }
    contains(t) { return this._get().includes(String(t)); }
    add(...ts) {
        const l = this._get(), had = this._e.hasAttribute(this._a);
        let ch = false;
        for (let t of ts) { t = String(t); if (!t || /\s/.test(t)) throw new DOMException('The token provided contains HTML space characters, which are not valid in tokens.', t ? 'InvalidCharacterError' : 'SyntaxError'); if (!l.includes(t)) { l.push(t); ch = true; } }
        if (ch || !had) this._set(l);
    }
    remove(...ts) {
        const l = this._get();
        const r = l.filter((x) => !ts.map(String).includes(x));
        if (r.length !== l.length || this._e.hasAttribute(this._a)) this._set(r);
    }
    toggle(t, force) {
        t = String(t);
        const has = this.contains(t);
        if (force === undefined ? has : !force) { if (has) this.remove(t); return false; }
        if (!has) this.add(t);
        return true;
    }
    replace(a, b) { const l = this._get(); const i = l.indexOf(String(a)); if (i < 0) return false; l[i] = String(b); this._set([...new Set(l)]); return true; }
    supports() { return true; }
    forEach(f, t) { this._get().forEach((x, i) => f.call(t, x, i, this)); }
    entries() { return this._get().entries(); }
    keys() { return this._get().keys(); }
    values() { return this._get().values(); }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
    toString() { return this.value; }
}
hidden(G, 'DOMTokenList', DOMTokenList);
const tokenLists = new WeakMap(), datasets = new WeakMap(), styles = new WeakMap();
function tokenList(el, attr) {
    let m = tokenLists.get(el);
    if (!m) tokenLists.set(el, (m = {}));
    return m[attr] || (m[attr] = new DOMTokenList(el, attr));
}
function dataset(el) {
    let d = datasets.get(el);
    if (d) return d;
    const name = (k) => 'data-' + kebab(String(k));
    d = new Proxy({}, {
        get(t, k) { if (typeof k !== 'string') return undefined; const v = el.getAttribute(name(k)); return v === null ? undefined : v; },
        set(t, k, v) { if (typeof k === 'string') el.setAttribute(name(k), String(v)); return true; },
        has(t, k) { return typeof k === 'string' && el.hasAttribute(name(k)); },
        deleteProperty(t, k) { if (typeof k === 'string') el.removeAttribute(name(k)); return true; },
        ownKeys() { return el.getAttributeNames().filter((a) => a.startsWith('data-')).map((a) => camel(a.slice(5))); },
        getOwnPropertyDescriptor(t, k) {
            if (typeof k !== 'string') return undefined;
            const v = el.getAttribute(name(k));
            return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
        },
    });
    datasets.set(el, d);
    return d;
}

/* CSSStyleDeclaration over the style="" attribute */
function parseDecls(text) {
    const out = [];
    if (!text) return out;
    let depth = 0, q = '', start = 0;
    const push = (s) => {
        const i = s.indexOf(':');
        if (i <= 0) return;
        let name = s.slice(0, i).trim();
        let value = s.slice(i + 1).trim();
        if (!name.startsWith('--')) name = name.toLowerCase();
        let prio = '';
        const m = /!\s*important\s*$/i.exec(value);
        if (m) { prio = 'important'; value = value.slice(0, m.index).trim(); }
        const j = out.findIndex((d) => d[0] === name);
        if (j >= 0) out.splice(j, 1);
        out.push([name, value, prio]);
    };
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (q) { if (c === '\\') i++; else if (c === q) q = ''; continue; }
        if (c === '"' || c === "'") q = c;
        else if (c === '(') depth++;
        else if (c === ')' && depth) depth--;
        else if (c === ';' && !depth) { push(text.slice(start, i)); start = i + 1; }
    }
    push(text.slice(start));
    return out;
}
const SHORT = { margin: ['top', 'right', 'bottom', 'left'], padding: ['top', 'right', 'bottom', 'left'] };
function cssName(k) {
    if (k === 'cssFloat') return 'float';
    if (k.startsWith('--')) return k;
    if (/^webkit[A-Z]/.test(k)) return '-' + kebab(k);
    return kebab(k);
}
const CSSStyleDeclaration = iface('CSSStyleDeclaration', null);
const styleMethods = {
    getPropertyValue(n) { n = String(n); if (!n.startsWith('--')) n = n.toLowerCase(); const d = this._decls().find((x) => x[0] === n); return d ? d[1] : ''; },
    getPropertyPriority(n) { const d = this._decls().find((x) => x[0] === String(n).toLowerCase()); return d ? d[2] : ''; },
    setProperty(n, v, prio) {
        n = String(n); if (!n.startsWith('--')) n = n.toLowerCase();
        if (v === null || v === undefined || v === '') { this.removeProperty(n); return; }
        const l = this._decls().filter((x) => x[0] !== n);
        l.push([n, String(v).trim(), prio === 'important' ? 'important' : '']);
        this._write(l);
    },
    removeProperty(n) {
        n = String(n); if (!n.startsWith('--')) n = n.toLowerCase();
        const l = this._decls();
        const d = l.find((x) => x[0] === n);
        const r = l.filter((x) => x[0] !== n && !(SHORT[n] && x[0].startsWith(n + '-')));
        if (r.length !== l.length) this._write(r);
        return d ? d[1] : '';
    },
    item(i) { const d = this._decls()[i]; return d ? d[0] : ''; },
};
function makeStyle(el) {
    const decls = () => parseDecls(el.getAttribute('style'));
    const write = (l) => el.setAttribute('style', l.map((d) => d[0] + ': ' + d[1] + (d[2] ? ' !important' : '') + ';').join(' '));
    const target = Object.create(CSSStyleDeclaration.prototype);
    return new Proxy(target, {
        get(t, k, r) {
            if (k === '_decls') return decls;
            if (k === '_write') return write;
            if (typeof k !== 'string') return Reflect.get(t, k, r);
            if (k in styleMethods) return styleMethods[k];
            if (k === 'cssText') return el.getAttribute('style') || '';
            if (k === 'length') return decls().length;
            if (/^[0-9]+$/.test(k)) { const d = decls()[+k]; return d ? d[0] : undefined; }
            if (k === 'parentRule') return null;
            if (k in t) return Reflect.get(t, k, r);
            const n = cssName(k);
            const d = decls().find((x) => x[0] === n);
            if (d) return d[1];
            if (SHORT[n]) { const parts = SHORT[n].map((s) => { const x = decls().find((y) => y[0] === n + '-' + s); return x ? x[1] : ''; }); return parts.every(Boolean) ? parts.join(' ') : ''; }
            return '';
        },
        set(t, k, v) {
            if (typeof k !== 'string') return true;
            if (k === 'cssText') { el.setAttribute('style', String(v)); return true; }
            const self = { _decls: decls, _write: write };
            self.removeProperty = (n) => styleMethods.removeProperty.call(self, n);
            styleMethods.setProperty.call(self, cssName(k), v === null || v === undefined ? '' : String(v));
            return true;
        },
        has(t, k) { return typeof k === 'string' ? true : k in t; },
    });
}
function boolAttr(proto, names) {
    for (const nm of names) {
        const attr = nm.toLowerCase();
        Object.defineProperty(proto, nm, {
            get() { return this.hasAttribute(attr); },
            set(v) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); },
            configurable: true, enumerable: true,
        });
    }
}
function strAttr(proto, names) {
    for (const nm of names) {
        const attr = nm === 'htmlFor' ? 'for' : nm === 'httpEquiv' ? 'http-equiv' : nm === 'acceptCharset' ? 'accept-charset' : nm.toLowerCase();
        Object.defineProperty(proto, nm, {
            get() { const v = this.getAttribute(attr); return v === null ? '' : v; },
            set(v) { this.setAttribute(attr, str(v)); },
            configurable: true, enumerable: true,
        });
    }
}
function urlAttr(proto, names) {
    for (const nm of names) {
        const attr = nm.toLowerCase();
        Object.defineProperty(proto, nm, {
            get() { const v = this.getAttribute(attr); if (v === null) return ''; try { return new URL(v, document.baseURI).href; } catch (e) { return v; } },
            set(v) { this.setAttribute(attr, str(v)); },
            configurable: true, enumerable: true,
        });
    }
}
function numAttr(proto, names, dflt) {
    for (const nm of names) {
        const attr = nm.toLowerCase();
        Object.defineProperty(proto, nm, {
            get() { const v = parseInt(this.getAttribute(attr), 10); return isNaN(v) ? dflt : v; },
            set(v) { this.setAttribute(attr, String(Math.trunc(Number(v)) || 0)); },
            configurable: true, enumerable: true,
        });
    }
}

class DOMRectReadOnly {
    constructor(x = 0, y = 0, w = 0, h = 0) { this.x = x; this.y = y; this.width = w; this.height = h; }
    get left() { return Math.min(this.x, this.x + this.width); }
    get top() { return Math.min(this.y, this.y + this.height); }
    get right() { return Math.max(this.x, this.x + this.width); }
    get bottom() { return Math.max(this.y, this.y + this.height); }
    toJSON() { return { x: this.x, y: this.y, width: this.width, height: this.height, top: this.top, right: this.right, bottom: this.bottom, left: this.left }; }
    static fromRect(r = {}) { return new this(r.x, r.y, r.width, r.height); }
}
class DOMRect extends DOMRectReadOnly {}
hidden(G, 'DOMRectReadOnly', DOMRectReadOnly);
hidden(G, 'DOMRect', DOMRect);
const box = (el) => N.rect(el);

def(Element.prototype, {
    get attributes() {
        const el = this;
        const l = el.getAttributeNames().map((n) => ({ name: n, localName: n, value: el.getAttribute(n), nodeName: n, nodeValue: el.getAttribute(n), specified: true, ownerElement: el, namespaceURI: null, prefix: null }));
        l.getNamedItem = (n) => l.find((a) => a.name === String(n).toLowerCase()) || null;
        l.item = (i) => l[i] || null;
        return l;
    },
    get namespaceURI() { return this instanceof SVGElement ? 'http://www.w3.org/2000/svg' : 'http://www.w3.org/1999/xhtml'; },
    get prefix() { return null; },
    getAttribute(n) { return N.attr(this, String(n)); },
    getAttributeNS(ns, n) { return N.attr(this, String(n)); },
    setAttribute(n, v) {
        n = String(n);
        if (!/^[^\s"'>\/=\x00-\x1f]+$/.test(n)) throw new DOMException("Failed to execute 'setAttribute' on 'Element': '" + n + "' is not a valid attribute name.", 'InvalidCharacterError');
        N.setAttr(this, n, String(v));
    },
    setAttributeNS(ns, n, v) { this.setAttribute(String(n).replace(/^.*:/, ''), v); },
    removeAttribute(n) { N.delAttr(this, String(n)); },
    removeAttributeNS(ns, n) { N.delAttr(this, String(n)); },
    hasAttribute(n) { return N.attr(this, String(n)) !== null; },
    hasAttributeNS(ns, n) { return N.attr(this, String(n)) !== null; },
    hasAttributes() { return N.attrNames(this).length > 0; },
    getAttributeNames() { return N.attrNames(this); },
    toggleAttribute(n, force) {
        const has = this.hasAttribute(n);
        if (force === undefined ? has : !force) { if (has) this.removeAttribute(n); return false; }
        if (!has) this.setAttribute(n, '');
        return true;
    },
    getAttributeNode(n) { const v = this.getAttribute(n); return v === null ? null : { name: String(n).toLowerCase(), value: v, specified: true, ownerElement: this }; },
    get id() { return this.getAttribute('id') || ''; },
    set id(v) { this.setAttribute('id', str(v)); },
    get className() { return this.getAttribute('class') || ''; },
    set className(v) { this.setAttribute('class', str(v)); },
    get classList() { return tokenList(this, 'class'); },
    set classList(v) { this.setAttribute('class', String(v)); },
    get slot() { return this.getAttribute('slot') || ''; },
    get innerHTML() { return N.html(this, false); },
    set innerHTML(v) { N.setHTML(this, v === null ? '' : String(v), 0); },
    get outerHTML() { return N.html(this, true); },
    set outerHTML(v) { if (this.parentNode) N.setHTML(this, String(v), 1); },
    insertAdjacentHTML(where, html) {
        const w = { beforebegin: 2, afterbegin: 3, beforeend: 4, afterend: 5 }[String(where).toLowerCase()];
        if (!w) throw new DOMException("Failed to execute 'insertAdjacentHTML' on 'Element': The value provided ('" + where + "') is not one of 'beforeBegin', 'afterBegin', 'beforeEnd', or 'afterEnd'.", 'SyntaxError');
        N.setHTML(this, String(html), w);
    },
    insertAdjacentElement(where, el) {
        switch (String(where).toLowerCase()) {
        case 'beforebegin': if (!this.parentNode) return null; this.parentNode.insertBefore(el, this); return el;
        case 'afterbegin': this.insertBefore(el, this.firstChild); return el;
        case 'beforeend': this.appendChild(el); return el;
        case 'afterend': if (!this.parentNode) return null; this.parentNode.insertBefore(el, this.nextSibling); return el;
        }
        throw new DOMException('bad position', 'SyntaxError');
    },
    insertAdjacentText(where, t) { this.insertAdjacentElement(where, document.createTextNode(String(t))); },
    matches(s) { return N.matches(this, String(s)); },
    webkitMatchesSelector(s) { return N.matches(this, String(s)); },
    msMatchesSelector(s) { return N.matches(this, String(s)); },
    closest(s) { return N.closest(this, String(s)); },
    getBoundingClientRect() {
        const r = box(this), v = N.view();
        return r ? new DOMRect(r[0] - v[0], r[1] - v[1], r[2], r[3]) : new DOMRect(0, 0, 0, 0);
    },
    getClientRects() { const r = box(this); if (!r) return []; const v = N.view(); return [new DOMRect(r[0] - v[0], r[1] - v[1], r[2], r[3])]; },
    get clientWidth() { const t = this.localName; if (t === 'html' || (t === 'body' && document.compatMode === 'BackCompat')) return N.view()[2]; const r = box(this); return r ? r[2] : 0; },
    get clientHeight() { const t = this.localName; if (t === 'html' || (t === 'body' && document.compatMode === 'BackCompat')) return N.view()[3]; const r = box(this); return r ? r[3] : 0; },
    get clientTop() { return 0; },
    get clientLeft() { return 0; },
    get scrollWidth() { const t = this.localName; if (t === 'html' || t === 'body') return N.view()[4]; const r = box(this); return r ? r[2] : 0; },
    get scrollHeight() { const t = this.localName; if (t === 'html' || t === 'body') return N.view()[5]; const r = box(this); return r ? r[3] : 0; },
    get scrollTop() { const t = this.localName; return t === 'html' || t === 'body' ? N.view()[1] : 0; },
    set scrollTop(v) { const t = this.localName; if (t === 'html' || t === 'body') N.scroll(N.view()[0], Number(v) || 0); },
    get scrollLeft() { const t = this.localName; return t === 'html' || t === 'body' ? N.view()[0] : 0; },
    set scrollLeft(v) { const t = this.localName; if (t === 'html' || t === 'body') N.scroll(Number(v) || 0, N.view()[1]); },
    scrollIntoView(opt) {
        const r = box(this); if (!r) return;
        const v = N.view();
        let y = r[1];
        if (opt === false || (opt && opt.block === 'end')) y = r[1] + r[3] - v[3];
        else if (opt && (opt.block === 'center' || opt.block === 'nearest')) y = r[1] - (v[3] - r[3]) / 2;
        N.scroll(v[0], y);
    },
    scrollTo() {}, scrollBy() {}, scroll() {},
    attachShadow() { throw new DOMException('Shadow DOM is not supported', 'NotSupportedError'); },
    get shadowRoot() { return null; },
    animate() { return { finished: Promise.resolve(), cancel() {}, play() {}, pause() {}, finish() {}, onfinish: null, addEventListener() {} }; },
    getAnimations() { return []; },
    requestFullscreen() { return Promise.reject(new DOMException('Not supported', 'NotSupportedError')); },
    setPointerCapture() {}, releasePointerCapture() {}, hasPointerCapture() { return false; },
});
def(HTMLElement.prototype, {
    get style() { let s = styles.get(this); if (!s) styles.set(this, (s = makeStyle(this))); return s; },
    set style(v) { this.setAttribute('style', String(v)); },
    get dataset() { return dataset(this); },
    get innerText() { return N.text(this); },
    set innerText(v) { N.setText(this, v === null || v === undefined ? '' : String(v)); },
    get outerText() { return N.text(this); },
    get offsetWidth() { const r = box(this); return r ? Math.round(r[2]) : 0; },
    get offsetHeight() { const r = box(this); return r ? Math.round(r[3]) : 0; },
    get offsetTop() { const r = box(this); return r ? r[1] : 0; },
    get offsetLeft() { const r = box(this); return r ? r[0] : 0; },
    get offsetParent() { return box(this) ? document.body : null; },
    get tabIndex() { const v = parseInt(this.getAttribute('tabindex'), 10); return isNaN(v) ? (/^(a|button|input|select|textarea)$/.test(this.localName) ? 0 : -1) : v; },
    set tabIndex(v) { this.setAttribute('tabindex', String(v)); },
    get isContentEditable() { return false; },
    get contentEditable() { return this.getAttribute('contenteditable') || 'inherit'; },
    set contentEditable(v) { this.setAttribute('contenteditable', String(v)); },
    focus() { if (focusable(this)) focusEl(this); },
    blur() { if (N.active() === this) focusEl(null); },
    click() { if (!this.hasAttribute('disabled') || !/^(button|input|select|textarea)$/.test(this.localName)) activate(this, 0, 0, 0, true); },
});
strAttr(HTMLElement.prototype, ['title', 'lang', 'dir', 'accessKey', 'autocapitalize', 'nonce', 'role', 'inputMode', 'enterKeyHint']);
boolAttr(HTMLElement.prototype, ['hidden', 'inert', 'draggable', 'spellcheck', 'translate', 'autofocus']);
def(SVGElement.prototype, {
    get style() { let s = styles.get(this); if (!s) styles.set(this, (s = makeStyle(this))); return s; },
    get dataset() { return dataset(this); },
    get ownerSVGElement() { return this.closest('svg'); },
    focus() {}, blur() {},
    getBBox() { const r = box(this); return r ? { x: 0, y: 0, width: r[2], height: r[3] } : { x: 0, y: 0, width: 0, height: 0 }; },
});

/* ---- elements with their own properties ----------------------------------------------------- */
const formOf = (el) => N.ctl(el, 20);
for (const t of ['a', 'area']) {
    const p = tagProtos[t];
    urlAttr(p, ['href']);
    strAttr(p, ['target', 'download', 'rel', 'hreflang', 'type', 'referrerPolicy', 'ping', 'coords', 'shape', 'name']);
    def(p, { get relList() { return tokenList(this, 'rel'); }, get text() { return this.textContent; }, set text(v) { this.textContent = v; }, toString() { return this.href; } });
    for (const k of ['protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash', 'origin', 'username', 'password']) {
        Object.defineProperty(p, k, {
            get() { try { return new URL(this.href)[k]; } catch (e) { return ''; } },
            set(v) { try { const u = new URL(this.href); u[k] = v; this.href = u.href; } catch (e) { /* ignore */ } },
            configurable: true, enumerable: true,
        });
    }
}
urlAttr(I('Image').prototype, ['src']);
strAttr(I('Image').prototype, ['alt', 'srcset', 'sizes', 'crossOrigin', 'useMap', 'loading', 'decoding', 'referrerPolicy', 'fetchPriority']);
boolAttr(I('Image').prototype, ['isMap']);
def(I('Image').prototype, {
    get width() { const v = parseInt(this.getAttribute('width'), 10); if (!isNaN(v)) return v; const r = box(this); return r ? r[2] : N.img(this)[0]; },
    set width(v) { this.setAttribute('width', String(v)); },
    get height() { const v = parseInt(this.getAttribute('height'), 10); if (!isNaN(v)) return v; const r = box(this); return r ? r[3] : N.img(this)[1]; },
    set height(v) { this.setAttribute('height', String(v)); },
    get naturalWidth() { return N.img(this)[0]; },
    get naturalHeight() { return N.img(this)[1]; },
    get complete() { return N.img(this)[2] !== 0; },
    get currentSrc() { return this.src; },
    decode() { return Promise.resolve(); },
});
const ctlProps = {
    get form() { return formOf(this); },
    get value() { return N.ctl(this, 0); },
    set value(v) { N.ctl(this, 1, v === null || v === undefined ? '' : String(v)); },
    get disabled() { return this.hasAttribute('disabled'); },
    set disabled(v) { if (v) this.setAttribute('disabled', ''); else this.removeAttribute('disabled'); },
    get labels() { const id = this.id; const l = id ? Array.from(document.querySelectorAll('label')).filter((x) => x.getAttribute('for') === id) : []; const p = this.closest('label'); if (p && !l.includes(p)) l.push(p); return staticList(l); },
    get willValidate() { return true; },
    get validity() { return validity(this); },
    get validationMessage() { const v = validity(this); return v.valid ? '' : this._custom || (v.valueMissing ? 'Please fill out this field.' : 'Please match the requested format.'); },
    checkValidity() { const ok = validity(this).valid; if (!ok) this.dispatchEvent(new Event('invalid', { cancelable: true })); return ok; },
    reportValidity() { return this.checkValidity(); },
    setCustomValidity(m) { hidden(this, '_custom', String(m)); },
};
function validity(el) {
    const v = el.value || '', t = (el.type || '').toLowerCase();
    const missing = el.hasAttribute('required') && !el.disabled && ((t === 'checkbox' || t === 'radio') ? !el.checked : v === '');
    let typeMismatch = false, patternMismatch = false, tooLong = false, tooShort = false, rangeUnderflow = false, rangeOverflow = false;
    if (v !== '') {
        if (t === 'email') typeMismatch = !/^[^\s@]+@[^\s@]+$/.test(v);
        else if (t === 'url') { try { new URL(v); } catch (e) { typeMismatch = true; } }
        else if (t === 'number') typeMismatch = isNaN(Number(v));
        const pat = el.getAttribute('pattern');
        if (pat) { try { patternMismatch = !new RegExp('^(?:' + pat + ')$', 'u').test(v); } catch (e) { /* ignore */ } }
        const ml = parseInt(el.getAttribute('maxlength'), 10), mn = parseInt(el.getAttribute('minlength'), 10);
        if (!isNaN(ml)) tooLong = v.length > ml;
        if (!isNaN(mn)) tooShort = v.length < mn;
        if (t === 'number' || t === 'range') {
            const n = Number(v), lo = parseFloat(el.getAttribute('min')), hi = parseFloat(el.getAttribute('max'));
            if (!isNaN(lo)) rangeUnderflow = n < lo;
            if (!isNaN(hi)) rangeOverflow = n > hi;
        }
    }
    const custom = !!el._custom;
    const valid = !(missing || typeMismatch || patternMismatch || tooLong || tooShort || rangeUnderflow || rangeOverflow || custom);
    return { valueMissing: missing, typeMismatch, patternMismatch, tooLong, tooShort, rangeUnderflow, rangeOverflow, stepMismatch: false, badInput: false, customError: custom, valid };
}
const inputP = I('Input').prototype;
def(inputP, ctlProps);
def(inputP, {
    get type() { const t = (this.getAttribute('type') || 'text').toLowerCase(); return t; },
    set type(v) { this.setAttribute('type', String(v)); },
    get checked() { return N.ctl(this, 2); },
    set checked(v) { N.ctl(this, 3, !!v); },
    get defaultValue() { return this.getAttribute('value') || ''; },
    set defaultValue(v) { this.setAttribute('value', String(v)); },
    get defaultChecked() { return this.hasAttribute('checked'); },
    set defaultChecked(v) { if (v) this.setAttribute('checked', ''); else this.removeAttribute('checked'); },
    get indeterminate() { return !!this._ind; },
    set indeterminate(v) { hidden(this, '_ind', !!v); },
    get valueAsNumber() { const n = parseFloat(this.value); return isNaN(n) ? NaN : n; },
    set valueAsNumber(n) { this.value = String(n); },
    get selectionStart() { return N.ctl(this, 11); },
    set selectionStart(v) { N.ctl(this, 12, Number(v) || 0); },
    get selectionEnd() { return N.ctl(this, 11); },
    set selectionEnd(v) { N.ctl(this, 12, Number(v) || 0); },
    get selectionDirection() { return 'none'; },
    get files() { return null; },
    get list() { const id = this.getAttribute('list'); return id ? document.getElementById(id) : null; },
    select() { N.ctl(this, 12, -1); },
    setSelectionRange(a, b) { N.ctl(this, 12, Number(b) || 0); },
    setRangeText() {},
    stepUp(n = 1) { this.value = String((parseFloat(this.value) || 0) + n * (parseFloat(this.step) || 1)); },
    stepDown(n = 1) { this.value = String((parseFloat(this.value) || 0) - n * (parseFloat(this.step) || 1)); },
    showPicker() {},
});
strAttr(inputP, ['name', 'placeholder', 'accept', 'alt', 'autocomplete', 'dirName', 'formAction', 'formEnctype', 'formMethod',
    'formTarget', 'max', 'min', 'pattern', 'step', 'inputMode']);
urlAttr(inputP, ['src']);
boolAttr(inputP, ['required', 'readOnly', 'multiple', 'formNoValidate']);
numAttr(inputP, ['maxLength', 'minLength'], -1);
numAttr(inputP, ['size'], 20);
const textareaP = I('TextArea').prototype;
def(textareaP, ctlProps);
def(textareaP, {
    get type() { return 'textarea'; },
    get defaultValue() { return this.textContent; },
    set defaultValue(v) { this.textContent = v; },
    get textLength() { return this.value.length; },
    get selectionStart() { return N.ctl(this, 11); },
    set selectionStart(v) { N.ctl(this, 12, Number(v) || 0); },
    get selectionEnd() { return N.ctl(this, 11); },
    set selectionEnd(v) { N.ctl(this, 12, Number(v) || 0); },
    select() { N.ctl(this, 12, -1); },
    setSelectionRange(a, b) { N.ctl(this, 12, Number(b) || 0); },
});
strAttr(textareaP, ['name', 'placeholder', 'wrap', 'autocomplete']);
boolAttr(textareaP, ['required', 'readOnly']);
numAttr(textareaP, ['rows'], 2);
numAttr(textareaP, ['cols'], 20);
numAttr(textareaP, ['maxLength', 'minLength'], -1);
const buttonP = I('Button').prototype;
def(buttonP, ctlProps);
def(buttonP, {
    get type() { const t = (this.getAttribute('type') || 'submit').toLowerCase(); return t === 'reset' || t === 'button' ? t : 'submit'; },
    set type(v) { this.setAttribute('type', String(v)); },
    get value() { return this.getAttribute('value') || ''; },
    set value(v) { this.setAttribute('value', String(v)); },
});
strAttr(buttonP, ['name', 'formAction', 'formMethod', 'formTarget', 'formEnctype']);
boolAttr(buttonP, ['formNoValidate']);
const selectP = I('Select').prototype;
def(selectP, ctlProps);
def(selectP, {
    get type() { return this.multiple ? 'select-multiple' : 'select-one'; },
    get options() { const s = this; const l = liveList(() => Array.from(N.query(s, 'option', true)), HTMLOptionsCollection.prototype); return l; },
    get length() { return N.query(this, 'option', true).length; },
    set length(n) { const o = N.query(this, 'option', true); for (let i = o.length - 1; i >= n; i--) o[i].remove(); },
    get selectedIndex() { return N.ctl(this, 4); },
    set selectedIndex(i) { N.ctl(this, 5, Number(i)); },
    get selectedOptions() { const i = this.selectedIndex, o = N.query(this, 'option', true); return staticList(i >= 0 && o[i] ? [o[i]] : []); },
    item(i) { return N.query(this, 'option', true)[i] || null; },
    namedItem(n) { return N.query(this, 'option', true).find((o) => o.id === n || o.getAttribute('name') === n) || null; },
    add(o, before) { if (typeof before === 'number') before = this.item(before); this.insertBefore(o, before || null); },
    remove(i) { if (i === undefined) { if (this.parentNode) N.remove(this); return; } const o = this.item(i); if (o) o.remove(); },
});
strAttr(selectP, ['name', 'autocomplete']);
boolAttr(selectP, ['required', 'multiple']);
numAttr(selectP, ['size'], 0);
const optionP = I('Option').prototype;
def(optionP, {
    get value() { const v = this.getAttribute('value'); return v === null ? this.text : v; },
    set value(v) { this.setAttribute('value', String(v)); },
    get text() { return this.textContent.replace(/\s+/g, ' ').trim(); },
    set text(v) { this.textContent = v; },
    get label() { const l = this.getAttribute('label'); return l === null ? this.text : l; },
    set label(v) { this.setAttribute('label', String(v)); },
    get index() { const s = this.closest('select'); return s ? N.query(s, 'option', true).indexOf(this) : 0; },
    get selected() { const s = this.closest('select'); return s ? s.selectedIndex === this.index : this.hasAttribute('selected'); },
    set selected(v) { const s = this.closest('select'); if (s && v) s.selectedIndex = this.index; else if (!s) this.defaultSelected = v; },
    get defaultSelected() { return this.hasAttribute('selected'); },
    set defaultSelected(v) { if (v) this.setAttribute('selected', ''); else this.removeAttribute('selected'); },
    get form() { const s = this.closest('select'); return s ? s.form : null; },
});
boolAttr(optionP, ['disabled']);
boolAttr(I('OptGroup').prototype, ['disabled']);
strAttr(I('OptGroup').prototype, ['label']);
const formP = I('Form').prototype;
def(formP, {
    get elements() { const f = this; return liveList(() => N.ctl(f, 21), HTMLCollection.prototype); },
    get length() { return N.ctl(this, 21).length; },
    get action() { const a = this.getAttribute('action'); try { return new URL(a === null ? '' : a, document.baseURI).href; } catch (e) { return a || ''; } },
    set action(v) { this.setAttribute('action', String(v)); },
    get method() { return (this.getAttribute('method') || 'get').toLowerCase() === 'post' ? 'post' : 'get'; },
    set method(v) { this.setAttribute('method', String(v)); },
    submit() { N.ctl(this, 9, null); },
    requestSubmit(s) { if (s && (!s.form || s.form !== this)) throw new DOMException('The specified element is not a submit button of this form.', 'NotFoundError'); submitForm(this, s || null, true); },
    reset() { if (this.dispatchEvent(new Event('reset', { bubbles: true, cancelable: true }))) N.ctl(this, 10); },
    checkValidity() { return Array.from(N.ctl(this, 21)).every((e) => !e.checkValidity || e.checkValidity()); },
    reportValidity() { return this.checkValidity(); },
    item(i) { return N.ctl(this, 21)[i] || null; },
    namedItem(n) { return N.ctl(this, 21).find((e) => e.id === n || e.getAttribute('name') === n) || null; },
});
strAttr(formP, ['name', 'target', 'enctype', 'encoding', 'acceptCharset', 'autocomplete', 'rel']);
boolAttr(formP, ['noValidate']);
const labelP = I('Label').prototype;
strAttr(labelP, ['htmlFor']);
def(labelP, {
    get control() { const f = this.getAttribute('for'); return f ? document.getElementById(f) : this.querySelector('input,select,textarea,button'); },
    get form() { const c = this.control; return c ? c.form : null; },
});
const scriptP = I('Script').prototype;
urlAttr(scriptP, ['src']);
strAttr(scriptP, ['type', 'charset', 'crossOrigin', 'integrity', 'referrerPolicy', 'event', 'htmlFor', 'fetchPriority']);
boolAttr(scriptP, ['defer']);
def(scriptP, {
    get async() { return this.hasAttribute('async') || N.ctl(this, 22); },
    set async(v) { if (v) this.setAttribute('async', ''); else this.removeAttribute('async'); N.ctl(this, 23, !!v); },
    get text() { return this.textContent; },
    set text(v) { this.textContent = v; },
});
const linkP = I('Link').prototype;
urlAttr(linkP, ['href']);
strAttr(linkP, ['rel', 'media', 'type', 'as', 'crossOrigin', 'hreflang', 'integrity', 'referrerPolicy', 'sizes', 'imageSrcset']);
def(linkP, { get relList() { return tokenList(this, 'rel'); }, get sheet() { return null; } });
boolAttr(linkP, ['disabled']);
class CSSStyleSheet {
    constructor(owner) { hidden(this, '_o', owner || null); hidden(this, '_r', null); this.disabled = false; }
    _rules() { if (!this._r) hidden(this, '_r', this._o ? [this._o.textContent] : []); return this._r; }
    get cssRules() { return this._rules().map((t) => ({ cssText: t, type: 1 })); }
    get rules() { return this.cssRules; }
    get ownerNode() { return this._o; }
    get href() { return null; }
    get media() { return { mediaText: this._o ? this._o.getAttribute('media') || '' : '' }; }
    insertRule(rule, i = 0) { const r = this._rules(); if (i > r.length) throw new DOMException('index', 'IndexSizeError'); r.splice(i, 0, String(rule)); this._flush(); return i; }
    deleteRule(i) { const r = this._rules(); r.splice(i, 1); this._flush(); }
    addRule(sel, st, i) { return this.insertRule(sel + '{' + st + '}', i === undefined ? this._rules().length : i); }
    removeRule(i) { this.deleteRule(i || 0); }
    replaceSync(t) { hidden(this, '_r', [String(t)]); this._flush(); }
    replace(t) { this.replaceSync(t); return Promise.resolve(this); }
    _flush() { if (this._o) N.setText(this._o, this._rules().join('\n')); }
}
hidden(G, 'CSSStyleSheet', CSSStyleSheet);
const sheets = new WeakMap();
const styleElP = I('Style').prototype;
strAttr(styleElP, ['media', 'type']);
def(styleElP, { get sheet() { let s = sheets.get(this); if (!s) sheets.set(this, (s = new CSSStyleSheet(this))); return s; } });
boolAttr(styleElP, ['disabled']);
const iframeP = I('IFrame').prototype;
urlAttr(iframeP, ['src']);
strAttr(iframeP, ['name', 'srcdoc', 'allow', 'width', 'height', 'loading', 'sandbox', 'referrerPolicy']);
def(iframeP, { get contentWindow() { return null; }, get contentDocument() { return null; } });
const canvasP = I('Canvas').prototype;
numAttr(canvasP, ['width'], 300);
numAttr(canvasP, ['height'], 150);
def(canvasP, {
    getContext() { return null; },
    toDataURL() { return 'data:,'; },
    toBlob(cb) { setTimeout(() => cb(null), 0); },
});
const mediaP = MediaElement.prototype;
urlAttr(mediaP, ['src']);
boolAttr(mediaP, ['autoplay', 'controls', 'loop', 'muted', 'playsInline']);
def(mediaP, {
    play() { return Promise.reject(new DOMException('Playing media is not supported', 'NotSupportedError')); },
    pause() {}, load() {},
    canPlayType() { return ''; },
    get paused() { return true; }, get ended() { return false; }, get duration() { return NaN; },
    get currentTime() { return 0; }, set currentTime(v) {},
    get volume() { return 1; }, set volume(v) {},
    get readyState() { return 0; }, get networkState() { return 3; }, get error() { return null; },
});
boolAttr(I('Details').prototype, ['open']);
const dialogP = I('Dialog').prototype;
boolAttr(dialogP, ['open']);
def(dialogP, {
    show() { this.setAttribute('open', ''); },
    showModal() { this.setAttribute('open', ''); },
    close(rv) { if (rv !== undefined) this.returnValue = String(rv); if (this.hasAttribute('open')) { this.removeAttribute('open'); this.dispatchEvent(new Event('close')); } },
});
const tplContent = new WeakMap();
def(I('Template').prototype, {
    get content() { let c = tplContent.get(this); if (!c) tplContent.set(this, (c = N.template(this))); return c; },
    get innerHTML() { return N.html(this.content, false); },
    set innerHTML(v) { const c = this.content; while (c.firstChild) N.remove(c.firstChild); N.setHTML(c, v === null ? '' : String(v), 4); },
});
strAttr(I('Meta').prototype, ['name', 'content', 'httpEquiv', 'charset', 'media']);
urlAttr(I('Base').prototype, ['href']);
strAttr(I('Base').prototype, ['target']);
def(I('Title').prototype, { get text() { return this.textContent; }, set text(v) { this.textContent = v; } });
numAttr(I('LI').prototype, ['value'], 0);
numAttr(I('OList').prototype, ['start'], 1);
boolAttr(I('OList').prototype, ['reversed']);
const progP = I('Progress').prototype;
def(progP, {
    get value() { return parseFloat(this.getAttribute('value')) || 0; }, set value(v) { this.setAttribute('value', String(v)); },
    get max() { return parseFloat(this.getAttribute('max')) || 1; }, set max(v) { this.setAttribute('max', String(v)); },
});
for (const t of ['td', 'th']) { numAttr(tagProtos[t], ['colSpan', 'rowSpan'], 1); def(tagProtos[t], { get cellIndex() { return this.parentNode ? Array.prototype.indexOf.call(this.parentNode.children, this) : -1; } }); }
def(tagProtos.tr, {
    get cells() { return this.children; },
    get rowIndex() { const t = this.closest('table'); return t ? Array.from(t.querySelectorAll('tr')).indexOf(this) : -1; },
    insertCell(i = -1) { const c = document.createElement('td'); const ref = i < 0 ? null : this.children[i] || null; this.insertBefore(c, ref); return c; },
    deleteCell(i) { const c = this.children[i]; if (c) c.remove(); },
});
def(tagProtos.table, {
    get rows() { const t = this; return liveList(() => Array.from(N.query(t, 'tr', true)), HTMLCollection.prototype); },
    get tBodies() { const t = this; return liveList(() => N.children(t).filter((e) => e.localName === 'tbody'), HTMLCollection.prototype); },
    get tHead() { return N.children(this).find((e) => e.localName === 'thead') || null; },
    get tFoot() { return N.children(this).find((e) => e.localName === 'tfoot') || null; },
    get caption() { return N.children(this).find((e) => e.localName === 'caption') || null; },
    insertRow(i = -1) {
        let body = N.children(this).filter((e) => e.localName === 'tbody').pop();
        if (!body) { body = document.createElement('tbody'); this.appendChild(body); }
        const r = document.createElement('tr');
        const rows = this.rows;
        if (i < 0 || i >= rows.length) body.appendChild(r); else rows[i].parentNode.insertBefore(r, rows[i]);
        return r;
    },
    deleteRow(i) { const r = this.rows[i < 0 ? this.rows.length - 1 : i]; if (r) r.remove(); },
    createTBody() { const b = document.createElement('tbody'); this.appendChild(b); return b; },
});
for (const t of ['thead', 'tbody', 'tfoot']) def(tagProtos[t], {
    get rows() { return this.children; },
    insertRow(i = -1) { const r = document.createElement('tr'); this.insertBefore(r, i < 0 ? null : this.children[i] || null); return r; },
    deleteRow(i) { const r = this.children[i]; if (r) r.remove(); },
});

/* ---- Document ------------------------------------------------------------------------------- */
const XHTML = 'http://www.w3.org/1999/xhtml', SVGNS = 'http://www.w3.org/2000/svg';
def(Document.prototype, {
    get documentElement() { return N.children(this)[0] || null; },
    get head() { return N.query(this, 'head', false); },
    get body() { const h = N.children(this)[0]; return h ? N.children(h).find((e) => e.localName === 'body' || e.localName === 'frameset') || null : null; },
    set body(b) { const old = this.body; if (old) old.replaceWith(b); else if (this.documentElement) this.documentElement.appendChild(b); },
    get title() { return N.title(); },
    set title(v) { N.title(String(v)); },
    get URL() { return N.href(); },
    get documentURI() { return N.href(); },
    get baseURI() { const b = N.query(this, 'base[href]', false); if (b) { try { return new URL(b.getAttribute('href'), N.href()).href; } catch (e) { /* ignore */ } } return N.href(); },
    get location() { return location; },
    set location(v) { location.href = v; },
    get domain() { try { return new URL(N.href()).hostname; } catch (e) { return ''; } },
    set domain(v) {},
    get referrer() { return N.info(4); },
    get cookie() { return N.cookie(); },
    set cookie(v) { N.cookie(String(v)); },
    get readyState() { return N.info(0); },
    get compatMode() { return N.info(1); },
    get currentScript() { return N.info(2); },
    get activeElement() { return N.active() || this.body; },
    get defaultView() { return G; },
    get characterSet() { return 'UTF-8'; },
    get charset() { return 'UTF-8'; },
    get inputEncoding() { return 'UTF-8'; },
    get contentType() { return 'text/html'; },
    get doctype() { return null; },
    get hidden() { return false; },
    get visibilityState() { return 'visible'; },
    get webkitVisibilityState() { return 'visible'; },
    get fullscreenElement() { return null; },
    get fullscreenEnabled() { return false; },
    get pointerLockElement() { return null; },
    get scrollingElement() { return this.documentElement; },
    get designMode() { return 'off'; }, set designMode(v) {},
    get dir() { return this.documentElement ? this.documentElement.dir : ''; },
    get lastModified() { return new Date().toLocaleString(); },
    get styleSheets() { return Array.from(N.query(this, 'style', true)).map((s) => s.sheet); },
    get forms() { const d = this; return liveList(() => Array.from(N.query(d, 'form', true)), HTMLCollection.prototype); },
    get images() { const d = this; return liveList(() => Array.from(N.query(d, 'img', true)), HTMLCollection.prototype); },
    get links() { const d = this; return liveList(() => Array.from(N.query(d, 'a[href],area[href]', true)), HTMLCollection.prototype); },
    get scripts() { const d = this; return liveList(() => Array.from(N.query(d, 'script', true)), HTMLCollection.prototype); },
    get anchors() { const d = this; return liveList(() => Array.from(N.query(d, 'a[name]', true)), HTMLCollection.prototype); },
    get all() { const d = this; return liveList(() => Array.from(N.query(d, '*', true)), HTMLCollection.prototype); },
    get implementation() { return implementation; },
    get fonts() { return fonts; },
    get timeline() { return { currentTime: N.now() }; },
    hasFocus() { return true; },
    getElementById(id) { return N.byId(this, String(id)); },
    getElementsByName(n) { n = String(n); const d = this; return liveList(() => Array.from(N.query(d, '[name]', true)).filter((e) => e.getAttribute('name') === n)); },
    createElement(name, opts) {
        name = String(name);
        if (!/^[A-Za-z][^\s"'>\/=\x00-\x1f]*$/.test(name)) throw new DOMException("Failed to execute 'createElement' on 'Document': The tag name provided ('" + name + "') is not a valid name.", 'InvalidCharacterError');
        const el = N.create(1, name.toLowerCase());
        if (opts && opts.is) el.setAttribute('is', String(opts.is));
        upgrade(el);
        return el;
    },
    createElementNS(ns, name) {
        name = String(name).replace(/^.*:/, '');
        const el = N.create(ns === SVGNS ? 2 : 1, ns === XHTML || ns === null || ns === undefined ? name.toLowerCase() : name);
        return el;
    },
    createTextNode(s) { return N.create(3, String(s)); },
    createComment(s) { return N.create(8, String(s)); },
    createDocumentFragment() { return N.create(11, ''); },
    createCDATASection(s) { return N.create(3, String(s)); },
    createProcessingInstruction() { return N.create(8, ''); },
    createAttribute(n) { return { name: String(n).toLowerCase(), value: '', specified: true }; },
    createEvent(type) {
        const t = String(type).toLowerCase().replace(/s$/, '');
        const C = { mouseevent: MouseEvent, keyboardevent: KeyboardEvent, uievent: UIEvent, customevent: CustomEvent,
            focusevent: FocusEvent, htmlevent: Event, event: Event, messageevent: MessageEvent, popstateevent: PopStateEvent,
            hashchangeevent: HashChangeEvent, inputevent: InputEvent, touchevent: UIEvent, errorevent: ErrorEvent }[t];
        if (!C) throw new DOMException("The provided event type ('" + type + "') is invalid.", 'NotSupportedError');
        const e = new C('');
        e.initMouseEvent = e.initKeyboardEvent = e.initUIEvent = function (type, bubbles, cancelable) { this.initEvent(type, bubbles, cancelable); };
        return e;
    },
    createRange() { return new Range(); },
    createTreeWalker(root, what = 0xFFFFFFFF, filter = null) { return new TreeWalker(root, what, filter); },
    createNodeIterator(root, what = 0xFFFFFFFF, filter = null) { return new TreeWalker(root, what, filter); },
    importNode(n, deep) { return n.cloneNode(!!deep); },
    adoptNode(n) { if (n.parentNode) n.remove(); return n; },
    elementFromPoint(x, y) { return N.hit(Number(x), Number(y)); },
    elementsFromPoint(x, y) { const e = N.hit(Number(x), Number(y)); const l = []; for (let n = e; n && n.nodeType === 1; n = n.parentNode) l.push(n); return l; },
    caretRangeFromPoint() { return null; },
    getSelection() { return selection; },
    execCommand() { return false; },
    queryCommandSupported() { return false; },
    queryCommandEnabled() { return false; },
    open() { return this; },
    close() {},
    write(...a) { N.write(a.join('')); },
    writeln(...a) { N.write(a.join('') + '\n'); },
    exitFullscreen() { return Promise.resolve(); },
    exitPointerLock() {},
    startViewTransition(cb) { const p = Promise.resolve().then(() => cb && cb()); return { finished: p, ready: p, updateCallbackDone: p, skipTransition() {} }; },
    get nodeName() { return '#document'; },
});
const fonts = { ready: Promise.resolve(), status: 'loaded', check() { return true; }, load() { return Promise.resolve([]); },
    add() {}, delete() {}, clear() {}, forEach() {}, addEventListener() {}, removeEventListener() {}, size: 0 };
fonts.ready = Promise.resolve(fonts);
const implementation = {
    hasFeature() { return true; },
    createHTMLDocument(title) { return new DOMParser().parseFromString('<!doctype html><html><head><title>' + (title || '') + '</title></head><body></body></html>', 'text/html'); },
    createDocument() { return implementation.createHTMLDocument(''); },
    createDocumentType(n, p, s) { return { name: n, publicId: p, systemId: s, nodeType: 10 }; },
};
/* a parsed document of its own: a fragment holding <html> (enough for the usual reading of it) */
class DOMParser {
    parseFromString(s, type) {
        const t = document.createElement('template');
        t.innerHTML = String(s).replace(/^\s*<!doctype[^>]*>/i, '');
        const frag = t.content;
        let html = frag.querySelector('html');
        if (!html) { html = document.createElement('html'); html.appendChild(document.createElement('head')); const b = document.createElement('body'); while (frag.firstChild) b.appendChild(frag.firstChild); html.appendChild(b); frag.appendChild(html); }
        if (!html.querySelector('head')) html.insertBefore(document.createElement('head'), html.firstChild);
        if (!html.querySelector('body')) html.appendChild(document.createElement('body'));
        const d = Object.create(frag);
        Object.defineProperties(d, {
            documentElement: { get: () => html }, head: { get: () => html.querySelector('head') }, body: { get: () => html.querySelector('body') },
            title: { get: () => { const ti = html.querySelector('title'); return ti ? ti.textContent : ''; } },
            createElement: { value: (n) => document.createElement(n) }, createTextNode: { value: (x) => document.createTextNode(x) },
            nodeType: { value: 9 }, readyState: { value: 'complete' },
        });
        return d;
    }
}
hidden(G, 'DOMParser', DOMParser);
class XMLSerializer { serializeToString(n) { return n.nodeType === 1 ? n.outerHTML : n.nodeType === 3 ? n.data : n.innerHTML || ''; } }
hidden(G, 'XMLSerializer', XMLSerializer);
const NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1,
    SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4, SHOW_CDATA_SECTION: 8, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256, SHOW_DOCUMENT_FRAGMENT: 1024 };
hidden(G, 'NodeFilter', NodeFilter);
class TreeWalker {
    constructor(root, what, filter) { this.root = root; this.whatToShow = what; this.filter = filter; this.currentNode = root; }
    _ok(n) {
        if (!((this.whatToShow >>> 0) & (1 << (n.nodeType - 1)))) return 3;
        const f = this.filter;
        if (!f) return 1;
        return typeof f === 'function' ? f(n) : f.acceptNode(n);
    }
    _next(n) {
        if (n.firstChild) return n.firstChild;
        while (n && n !== this.root) { if (n.nextSibling) return n.nextSibling; n = n.parentNode; }
        return null;
    }
    nextNode() {
        let n = this.currentNode;
        while ((n = this._next(n))) { if (this._ok(n) === 1) { this.currentNode = n; return n; } }
        return null;
    }
    previousNode() {
        const all = [];
        for (let n = this.root; n; n = this._next(n)) all.push(n);
        for (let i = all.indexOf(this.currentNode) - 1; i >= 0; i--) if (all[i] !== this.root && this._ok(all[i]) === 1) { this.currentNode = all[i]; return all[i]; }
        return null;
    }
    firstChild() { for (let n = this.currentNode.firstChild; n; n = n.nextSibling) if (this._ok(n) === 1) { this.currentNode = n; return n; } return null; }
    lastChild() { for (let n = this.currentNode.lastChild; n; n = n.previousSibling) if (this._ok(n) === 1) { this.currentNode = n; return n; } return null; }
    nextSibling() { for (let n = this.currentNode.nextSibling; n; n = n.nextSibling) if (this._ok(n) === 1) { this.currentNode = n; return n; } return null; }
    previousSibling() { for (let n = this.currentNode.previousSibling; n; n = n.previousSibling) if (this._ok(n) === 1) { this.currentNode = n; return n; } return null; }
    parentNode() { const p = this.currentNode.parentNode; if (p && p !== this.root.parentNode) { this.currentNode = p; return p; } return null; }
    get referenceNode() { return this.currentNode; }
    detach() {}
}
hidden(G, 'TreeWalker', TreeWalker);
class Range {
    constructor() { this.startContainer = this.endContainer = document; this.startOffset = this.endOffset = 0; this.collapsed = true; this.commonAncestorContainer = document; }
    setStart(n, o) { this.startContainer = n; this.startOffset = o; }
    setEnd(n, o) { this.endContainer = n; this.endOffset = o; }
    setStartBefore(n) { this.setStart(n.parentNode, 0); }
    setStartAfter(n) { this.setStart(n.parentNode, 0); }
    setEndBefore(n) { this.setEnd(n.parentNode, 0); }
    setEndAfter(n) { this.setEnd(n.parentNode, 0); }
    selectNode(n) { this.startContainer = this.endContainer = n; }
    selectNodeContents(n) { this.startContainer = this.endContainer = n; }
    collapse() { this.collapsed = true; }
    cloneRange() { const r = new Range(); Object.assign(r, this); return r; }
    detach() {}
    deleteContents() {}
    getBoundingClientRect() { return this.startContainer.nodeType === 1 ? this.startContainer.getBoundingClientRect() : new DOMRect(); }
    getClientRects() { return []; }
    createContextualFragment(html) {
        const t = document.createElement('template');
        t.innerHTML = String(html);
        const f = t.content;
        for (const s of f.querySelectorAll('script')) N.ctl(s, 24);   /* scripts in it may run */
        return f;
    }
    toString() { return ''; }
}
hidden(G, 'Range', Range);
const selection = { rangeCount: 0, isCollapsed: true, type: 'None', anchorNode: null, focusNode: null,
    getRangeAt() { return new Range(); }, removeAllRanges() {}, addRange() {}, collapse() {}, collapseToEnd() {},
    collapseToStart() {}, selectAllChildren() {}, toString() { return ''; }, containsNode() { return false; }, empty() {} };
hidden(G, 'getSelection', () => selection);

/* custom elements: definitions are kept; elements made with createElement get the class's
   prototype and connectedCallback (no shadow DOM) */
const ceDefs = new Map(), ceWait = new Map();
function upgrade(el) {
    const d = ceDefs.get(el.localName);
    if (!d || Object.getPrototypeOf(el) === d.prototype) return;
    Object.setPrototypeOf(el, d.prototype);
}
const customElements = {
    define(name, C) {
        name = String(name);
        if (ceDefs.has(name)) throw new DOMException("the name '" + name + "' has already been used with this registry", 'NotSupportedError');
        ceDefs.set(name, C);
        for (const el of document.querySelectorAll(name)) {
            upgrade(el);
            try { if (typeof el.connectedCallback === 'function') el.connectedCallback(); } catch (e) { report(e); }
        }
        const w = ceWait.get(name);
        if (w) { w.forEach((f) => f(C)); ceWait.delete(name); }
    },
    get(name) { return ceDefs.get(String(name)); },
    getName(C) { for (const [k, v] of ceDefs) if (v === C) return k; return null; },
    whenDefined(name) { name = String(name); if (ceDefs.has(name)) return Promise.resolve(ceDefs.get(name)); return new Promise((r) => { if (!ceWait.has(name)) ceWait.set(name, []); ceWait.get(name).push(r); }); },
    upgrade(root) { for (const el of root.querySelectorAll('*')) upgrade(el); },
};
hidden(G, 'customElements', customElements);

/* ---- window ---------------------------------------------------------------------------------- */
Object.setPrototypeOf(G, Window.prototype);
for (const k of ['window', 'self', 'frames', 'globalThis']) hidden(G, k, G);
hidden(G, 'parent', G);
hidden(G, 'top', G);
hidden(G, 'opener', null);
hidden(G, 'frameElement', null);
hidden(G, 'closed', false);
hidden(G, 'name', '');
hidden(G, 'length', 0);
hidden(G, 'status', '');
hidden(G, 'origin', (() => { try { return new URL(N.href()).origin; } catch (e) { return 'null'; } })());
hidden(G, 'isSecureContext', /^https:/.test(N.href()));
hidden(G, 'crossOriginIsolated', false);
const viewGetter = (i) => ({ get() { return N.view()[i]; }, configurable: true, enumerable: true });
Object.defineProperties(G, {
    innerWidth: viewGetter(2), innerHeight: viewGetter(3), outerWidth: viewGetter(2), outerHeight: viewGetter(3),
    scrollX: viewGetter(0), scrollY: viewGetter(1), pageXOffset: viewGetter(0), pageYOffset: viewGetter(1),
    screenX: { value: 0, writable: true, configurable: true }, screenY: { value: 0, writable: true, configurable: true },
    screenLeft: { value: 0, writable: true, configurable: true }, screenTop: { value: 0, writable: true, configurable: true },
    devicePixelRatio: { value: 1, writable: true, configurable: true },
});
const scrollFn = (rel) => function (x, y) {
    const v = N.view();
    if (x && typeof x === 'object') { y = x.top; x = x.left; }
    if (x === undefined || x === null || isNaN(x)) x = rel ? 0 : v[0];
    if (y === undefined || y === null || isNaN(y)) y = rel ? 0 : v[1];
    N.scroll(rel ? v[0] + Number(x) : Number(x), rel ? v[1] + Number(y) : Number(y));
};
def(Window.prototype, {
    scrollTo: scrollFn(false), scroll: scrollFn(false), scrollBy: scrollFn(true),
    alert(m) { return N.dialog(0, m === undefined ? '' : String(m), ''); },
    confirm(m) { return !!N.dialog(1, m === undefined ? '' : String(m), ''); },
    prompt(m, d) { return N.dialog(2, m === undefined ? '' : String(m), d === undefined ? '' : String(d)); },
    print() {}, stop() {}, focus() {}, blur() {}, close() {}, moveTo() {}, moveBy() {}, resizeTo() {}, resizeBy() {},
    open(url, target) { if (url !== undefined && url !== '') N.open(new URL(String(url), document.baseURI).href); return null; },
    getComputedStyle(el, pseudo) { return computedStyle(el, pseudo); },
    matchMedia(q) { return new MediaQueryList(String(q)); },
    postMessage(data, origin) {
        let d;
        try { d = structuredClone(data); } catch (e) { d = data; }
        setTimeout(() => dispatch(G, new MessageEvent('message', { data: d, origin: G.origin, source: G })), 0);
    },
    requestIdleCallback(cb, opts) {
        const t0 = N.now();
        return setTimeout(() => cb({ didTimeout: false, timeRemaining: () => Math.max(0, 15 - (N.now() - t0)) }), 1);
    },
    cancelIdleCallback(id) { clearTimeout(id); },
    captureEvents() {}, releaseEvents() {},
    getSelection() { return selection; },
    find() { return false; },
});
/* computed style: the values the browser's own styling found (a few properties) */
function computedStyle(el, pseudo) {
    const get = (n) => (el instanceof Element && !pseudo ? N.style(el, n) : '');
    const target = Object.create(CSSStyleDeclaration.prototype);
    return new Proxy(target, {
        get(t, k) {
            if (k === 'getPropertyValue') return (n) => get(String(n));
            if (k === 'getPropertyPriority') return () => '';
            if (k === 'item') return () => '';
            if (k === 'length') return 0;
            if (k === 'cssText') return '';
            if (k === 'setProperty' || k === 'removeProperty') return () => { throw new DOMException('read-only', 'NoModificationAllowedError'); };
            if (typeof k !== 'string') return undefined;
            return get(cssName(k));
        },
    });
}
class MediaQueryList extends EventTarget {
    constructor(q) { super(); this.media = q; this.onchange = null; }
    get matches() { return N.media(this.media); }
    addListener(f) { this.addEventListener('change', f); }
    removeListener(f) { this.removeEventListener('change', f); }
}
hidden(G, 'MediaQueryList', MediaQueryList);

/* timers */
def(G, {
    setTimeout(fn, ms, ...a) { return N.timer(typeof fn === 'function' ? fn : compileHandler(String(fn)), Number(ms) || 0, false, a); },
    setInterval(fn, ms, ...a) { return N.timer(typeof fn === 'function' ? fn : compileHandler(String(fn)), Number(ms) || 0, true, a); },
    clearTimeout(id) { if (id !== undefined && id !== null) N.clear(Number(id)); },
    clearInterval(id) { if (id !== undefined && id !== null) N.clear(Number(id)); },
    requestAnimationFrame(cb) { if (typeof cb !== 'function') throw new TypeError("Failed to execute 'requestAnimationFrame' on 'Window': The callback provided as parameter 1 is not a function."); return N.raf(cb); },
    cancelAnimationFrame(id) { N.clear(Number(id)); },
    setImmediate(fn, ...a) { return N.timer(fn, 0, false, a); },
    clearImmediate(id) { N.clear(Number(id)); },
});
delete G.setImmediate;
delete G.clearImmediate;

/* location */
class Location {
    get href() { return N.href(); }
    set href(v) { N.nav(new URL(String(v), document.baseURI).href, 0); }
    get protocol() { return new URL(N.href()).protocol; }
    get host() { return new URL(N.href()).host; }
    get hostname() { return new URL(N.href()).hostname; }
    get port() { return new URL(N.href()).port; }
    get pathname() { return new URL(N.href()).pathname; }
    get search() { return new URL(N.href()).search; }
    get hash() { return new URL(N.href()).hash; }
    get origin() { return new URL(N.href()).origin; }
    get ancestorOrigins() { return []; }
    set protocol(v) { const u = new URL(N.href()); u.protocol = v; this.href = u.href; }
    set host(v) { const u = new URL(N.href()); u.host = v; this.href = u.href; }
    set hostname(v) { const u = new URL(N.href()); u.hostname = v; this.href = u.href; }
    set port(v) { const u = new URL(N.href()); u.port = v; this.href = u.href; }
    set pathname(v) { const u = new URL(N.href()); u.pathname = v; this.href = u.href; }
    set search(v) { const u = new URL(N.href()); u.search = v; this.href = u.href; }
    set hash(v) { const u = new URL(N.href()); v = String(v); u.hash = v; N.nav(u.href, 3); }
    assign(u) { N.nav(new URL(String(u), document.baseURI).href, 0); }
    replace(u) { N.nav(new URL(String(u), document.baseURI).href, 1); }
    reload() { N.nav(N.href(), 2); }
    toString() { return N.href(); }
    valueOf() { return this; }
}
const location = new Location();
Object.defineProperty(G, 'location', { get() { return location; }, set(v) { location.href = v; }, configurable: true, enumerable: true });
hidden(G, 'Location', Location);

/* history: same-document entries keep their state here, by history position */
const states = new Map();
class History {
    get length() { return N.hist(0); }
    get state() { const v = states.get(N.hist(4)); return v === undefined ? null : v; }
    get scrollRestoration() { return 'auto'; }
    set scrollRestoration(v) {}
    pushState(state, title, url) { this._set(state, url, 2); }
    replaceState(state, title, url) { this._set(state, url, 3); }
    _set(state, url, op) {
        let s = state;
        try { s = structuredClone(state); } catch (e) { throw new DOMException('The object could not be cloned.', 'DataCloneError'); }
        let u = N.href();
        if (url !== undefined && url !== null) {
            u = new URL(String(url), document.baseURI).href;
            if (new URL(u).origin !== new URL(N.href()).origin) throw new DOMException("Failed to execute 'pushState' on 'History': A history state object with URL '" + u + "' cannot be created in a document with origin '" + G.origin + "'.", 'SecurityError');
        }
        N.hist(op, u);
        states.set(N.hist(4), s === undefined ? null : s);
    }
    back() { N.hist(1, -1); }
    forward() { N.hist(1, 1); }
    go(d) { d = Number(d) || 0; if (d) N.hist(1, d); else location.reload(); }
}
const history = new History();
hidden(G, 'history', history);
hidden(G, 'History', History);

/* navigator, screen */
const UA = 'Mozilla/5.0 (NovaOS 2.5; i686) AppleWebKit/537.36 (KHTML, like Gecko) NovaBrowser/2.5 Safari/537.36';
const navigator = {
    userAgent: UA, appVersion: UA.slice(8), appName: 'Netscape', appCodeName: 'Mozilla', product: 'Gecko', productSub: '20030107',
    vendor: 'NovaOS', vendorSub: '', platform: 'Linux i686', language: 'en-US', languages: ['en-US', 'en'], onLine: true,
    cookieEnabled: true, doNotTrack: null, hardwareConcurrency: 1, maxTouchPoints: 0, deviceMemory: 1, pdfViewerEnabled: false,
    webdriver: false, plugins: [], mimeTypes: [],
    userAgentData: { brands: [{ brand: 'NovaBrowser', version: '2.5' }], mobile: false, platform: 'NovaOS', getHighEntropyValues() { return Promise.resolve({}); } },
    javaEnabled() { return false; },
    sendBeacon(url, data) { try { fetch(new URL(String(url), document.baseURI).href, { method: 'POST', body: data }).catch(() => {}); } catch (e) { return false; } return true; },
    clipboard: { writeText(t) { N.clip(String(t)); return Promise.resolve(); }, readText() { return Promise.resolve(N.clip()); } },
    permissions: { query() { return Promise.resolve({ state: 'denied', addEventListener() {}, onchange: null }); } },
    serviceWorker: undefined,
    connection: { effectiveType: '4g', downlink: 10, rtt: 50, saveData: false, addEventListener() {}, removeEventListener() {} },
    storage: { estimate() { return Promise.resolve({ quota: 2500000, usage: 0 }); }, persist() { return Promise.resolve(false); } },
    mediaDevices: undefined,
    share: undefined,
    vibrate() { return false; },
    registerProtocolHandler() {},
};
delete navigator.serviceWorker;
delete navigator.mediaDevices;
delete navigator.share;
hidden(G, 'navigator', navigator);
hidden(G, 'clientInformation', navigator);
const screen = {
    get width() { return 1024; }, get height() { return 768; }, get availWidth() { return 1024; }, get availHeight() { return 720; },
    colorDepth: 24, pixelDepth: 24, availLeft: 0, availTop: 0,
    orientation: { type: 'landscape-primary', angle: 0, addEventListener() {}, removeEventListener() {} },
};
hidden(G, 'screen', screen);
const performance_ = {
    now: () => N.now(),
    timeOrigin: Date.now() - N.now(),
    mark(n) { return { name: n, startTime: N.now(), duration: 0, entryType: 'mark' }; },
    measure(n) { return { name: n, startTime: 0, duration: 0, entryType: 'measure' }; },
    clearMarks() {}, clearMeasures() {}, clearResourceTimings() {}, setResourceTimingBufferSize() {},
    getEntries() { return []; }, getEntriesByType() { return []; }, getEntriesByName() { return []; },
    toJSON() { return {}; },
    timing: { navigationStart: Date.now() - N.now(), fetchStart: Date.now() - N.now(), responseEnd: Date.now() - N.now() },
    navigation: { type: 0, redirectCount: 0 },
    memory: { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 0 },
    addEventListener() {}, removeEventListener() {},
};
hidden(G, 'performance', performance_);
class PerformanceObserver { constructor(cb) {} observe() {} disconnect() {} takeRecords() { return []; } }
PerformanceObserver.supportedEntryTypes = [];
hidden(G, 'PerformanceObserver', PerformanceObserver);
const crypto_ = {
    getRandomValues(a) {
        if (!ArrayBuffer.isView(a)) throw new TypeError("Failed to execute 'getRandomValues' on 'Crypto': parameter 1 is not of type 'ArrayBufferView'.");
        if (a.byteLength > 65536) throw new DOMException('quota', 'QuotaExceededError');
        new Uint8Array(a.buffer, a.byteOffset, a.byteLength).set(N.random(a.byteLength));
        return a;
    },
    randomUUID() {
        const b = N.random(16);
        b[6] = (b[6] & 0x0f) | 0x40; b[8] = (b[8] & 0x3f) | 0x80;
        const h = Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
        return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
    },
};
hidden(G, 'crypto', crypto_);
const CSS = { supports(a, b) { return N.supports(b === undefined ? String(a) : String(a) + ':' + String(b)); }, escape(s) { return String(s).replace(/([^\w-])/g, '\\$1').replace(/^(\d)/, '\\3$1 '); } };
hidden(G, 'CSS', CSS);
hidden(G, 'styleMedia', { type: 'screen', matchMedium: (q) => N.media(String(q)) });

/* storage: N.storage gives the host object (getItem ... on its prototype) */
const Storage = iface('Storage', null);
def(Storage.prototype, {
    getItem(k) { return N.store(this, 0, String(k)); },
    setItem(k, v) { if (!N.store(this, 1, String(k), String(v))) throw new DOMException("Failed to execute 'setItem' on 'Storage': Setting the value of '" + k + "' exceeded the quota.", 'QuotaExceededError'); },
    removeItem(k) { N.store(this, 2, String(k)); },
    clear() { N.store(this, 3); },
    key(i) { return N.store(this, 4, Number(i)); },
    get length() { return N.store(this, 5); },
});
for (const [nm, kind] of [['localStorage', 0], ['sessionStorage', 1]]) {
    let s = null;
    Object.defineProperty(G, nm, { get() { if (!s) s = N.storage(kind, Storage.prototype); return s; }, configurable: true, enumerable: true });
}

/* ---- networking --------------------------------------------------------------------------- */
class Headers {
    constructor(init) {
        hidden(this, '_h', []);
        if (init instanceof Headers) init.forEach((v, k) => this.append(k, v));
        else if (Array.isArray(init)) for (const [k, v] of init) this.append(k, v);
        else if (init && typeof init === 'object') for (const k of Object.keys(init)) this.append(k, init[k]);
    }
    append(k, v) { this._h.push([String(k).toLowerCase(), String(v).trim()]); }
    set(k, v) { k = String(k).toLowerCase(); this.delete(k); this._h.push([k, String(v).trim()]); }
    get(k) { k = String(k).toLowerCase(); const l = this._h.filter((x) => x[0] === k).map((x) => x[1]); return l.length ? l.join(', ') : null; }
    getSetCookie() { return []; }
    has(k) { k = String(k).toLowerCase(); return this._h.some((x) => x[0] === k); }
    delete(k) { k = String(k).toLowerCase(); this._h = this._h.filter((x) => x[0] !== k); hidden(this, '_h', this._h); }
    forEach(f, t) { for (const [k, v] of this.entries()) f.call(t, v, k, this); }
    *entries() { const seen = new Set(); for (const [k] of [...this._h].sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0))) { if (!seen.has(k)) { seen.add(k); yield [k, this.get(k)]; } } }
    *keys() { for (const [k] of this.entries()) yield k; }
    *values() { for (const [, v] of this.entries()) yield v; }
    [Symbol.iterator]() { return this.entries(); }
}
hidden(G, 'Headers', Headers);
const te = new TextEncoder(), td = new TextDecoder();
class Blob {
    constructor(parts = [], opts = {}) {
        const chunks = [];
        for (const p of parts) {
            if (p instanceof Blob) chunks.push(p._b);
            else if (p instanceof ArrayBuffer) chunks.push(new Uint8Array(p));
            else if (ArrayBuffer.isView(p)) chunks.push(new Uint8Array(p.buffer, p.byteOffset, p.byteLength));
            else chunks.push(te.encode(String(p)));
        }
        let n = 0;
        for (const c of chunks) n += c.length;
        const b = new Uint8Array(n);
        let o = 0;
        for (const c of chunks) { b.set(c, o); o += c.length; }
        hidden(this, '_b', b);
        this.type = opts.type ? String(opts.type).toLowerCase() : '';
    }
    get size() { return this._b.length; }
    slice(a = 0, e = this._b.length, type = '') { return new Blob([this._b.slice(a, e)], { type }); }
    text() { return Promise.resolve(td.decode(this._b)); }
    arrayBuffer() { return Promise.resolve(this._b.slice().buffer); }
    bytes() { return Promise.resolve(this._b.slice()); }
    stream() { throw new DOMException('Streams are not supported', 'NotSupportedError'); }
}
class File extends Blob {
    constructor(parts, name, opts = {}) { super(parts, opts); this.name = String(name); this.lastModified = opts.lastModified || Date.now(); }
}
hidden(G, 'Blob', Blob);
hidden(G, 'File', File);
class FileReader extends EventTarget {
    constructor() { super(); this.readyState = 0; this.result = null; this.error = null; }
    _read(b, how) {
        this.readyState = 1;
        Promise.resolve().then(() => {
            const bytes = b._b;
            if (how === 0) this.result = td.decode(bytes);
            else if (how === 1) this.result = bytes.slice().buffer;
            else this.result = 'data:' + (b.type || 'application/octet-stream') + ';base64,' + btoa(Array.from(bytes, (x) => String.fromCharCode(x)).join(''));
            this.readyState = 2;
            for (const t of ['load', 'loadend']) { const e = new ProgressEvent(t, { loaded: bytes.length, total: bytes.length }); const h = this['on' + t]; if (typeof h === 'function') h.call(this, e); this.dispatchEvent(e); }
        });
    }
    readAsText(b) { this._read(b, 0); }
    readAsArrayBuffer(b) { this._read(b, 1); }
    readAsDataURL(b) { this._read(b, 2); }
    readAsBinaryString(b) { this._read(b, 0); }
    abort() {}
}
hidden(G, 'FileReader', FileReader);
const blobURLs = new Map();
let blobN = 0;
URL.createObjectURL = (b) => { const u = 'blob:' + G.origin + '/' + (++blobN); blobURLs.set(u, b); return u; };
URL.revokeObjectURL = (u) => { blobURLs.delete(String(u)); };
class FormData {
    constructor(form) {
        hidden(this, '_d', []);
        if (form instanceof Element) {
            for (const e of N.ctl(form, 21)) {
                const n = e.getAttribute('name');
                if (!n || e.disabled) continue;
                const t = (e.type || '').toLowerCase();
                if ((t === 'checkbox' || t === 'radio') && !e.checked) continue;
                if (t === 'submit' || t === 'button' || t === 'reset' || t === 'image' || t === 'file') continue;
                this._d.push([n, e.value]);
            }
        }
    }
    append(k, v, fn) { this._d.push([String(k), v instanceof Blob ? v : String(v)]); }
    set(k, v) { k = String(k); const i = this._d.findIndex((x) => x[0] === k); const val = v instanceof Blob ? v : String(v); if (i < 0) this._d.push([k, val]); else { this._d[i] = [k, val]; hidden(this, '_d', this._d.filter((x, j) => j <= i || x[0] !== k)); } }
    get(k) { const x = this._d.find((y) => y[0] === String(k)); return x ? x[1] : null; }
    getAll(k) { return this._d.filter((y) => y[0] === String(k)).map((y) => y[1]); }
    has(k) { return this._d.some((y) => y[0] === String(k)); }
    delete(k) { hidden(this, '_d', this._d.filter((y) => y[0] !== String(k))); }
    forEach(f, t) { for (const [k, v] of this._d) f.call(t, v, k, this); }
    *entries() { yield* this._d.map((x) => [x[0], x[1]]); }
    *keys() { for (const x of this._d) yield x[0]; }
    *values() { for (const x of this._d) yield x[1]; }
    [Symbol.iterator]() { return this.entries(); }
}
hidden(G, 'FormData', FormData);
class AbortSignal extends EventTarget {
    constructor() { super(); this.aborted = false; this.reason = undefined; this.onabort = null; }
    throwIfAborted() { if (this.aborted) throw this.reason; }
    static abort(r) { const c = new AbortController(); c.abort(r); return c.signal; }
    static timeout(ms) { const c = new AbortController(); setTimeout(() => c.abort(new DOMException('signal timed out', 'TimeoutError')), ms); return c.signal; }
    static any(list) { const c = new AbortController(); for (const s of list) { if (s.aborted) { c.abort(s.reason); break; } s.addEventListener('abort', () => c.abort(s.reason)); } return c.signal; }
}
class AbortController {
    constructor() { this.signal = new AbortSignal(); }
    abort(reason) {
        const s = this.signal;
        if (s.aborted) return;
        s.aborted = true;
        s.reason = reason === undefined ? new DOMException('signal is aborted without reason', 'AbortError') : reason;
        const e = new Event('abort');
        if (typeof s.onabort === 'function') s.onabort.call(s, e);
        s.dispatchEvent(e);
    }
}
hidden(G, 'AbortSignal', AbortSignal);
hidden(G, 'AbortController', AbortController);

function bodyBytes(body, headers) {
    if (body === undefined || body === null) return null;
    if (typeof body === 'string') { if (!headers.has('content-type')) headers.set('content-type', 'text/plain;charset=UTF-8'); return te.encode(body); }
    if (body instanceof URLSearchParams) { if (!headers.has('content-type')) headers.set('content-type', 'application/x-www-form-urlencoded;charset=UTF-8'); return te.encode(body.toString()); }
    if (body instanceof FormData) {
        const b = '----NovaFormBoundary' + Math.random().toString(36).slice(2);
        const parts = [];
        for (const [k, v] of body._d) {
            if (v instanceof Blob) parts.push('--' + b + '\r\nContent-Disposition: form-data; name="' + k + '"; filename="' + (v.name || 'blob') + '"\r\nContent-Type: ' + (v.type || 'application/octet-stream') + '\r\n\r\n', v, '\r\n');
            else parts.push('--' + b + '\r\nContent-Disposition: form-data; name="' + k + '"\r\n\r\n' + v + '\r\n');
        }
        parts.push('--' + b + '--\r\n');
        headers.set('content-type', 'multipart/form-data; boundary=' + b);
        return new Blob(parts)._b;
    }
    if (body instanceof Blob) { if (body.type && !headers.has('content-type')) headers.set('content-type', body.type); return body._b; }
    if (body instanceof ArrayBuffer) return new Uint8Array(body);
    if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
    return te.encode(String(body));
}
class Response {
    constructor(body, init = {}) {
        const h = new Headers(init.headers);
        hidden(this, '_b', body instanceof Uint8Array ? body : bodyBytes(body, h) || new Uint8Array(0));
        this.status = init.status === undefined ? 200 : init.status;
        this.statusText = init.statusText || '';
        this.headers = h;
        this.ok = this.status >= 200 && this.status < 300;
        this.url = init.url || '';
        this.redirected = !!init.redirected;
        this.type = init.type || 'default';
        this.bodyUsed = false;
    }
    _take() { if (this.bodyUsed) return Promise.reject(new TypeError('Failed to execute: body stream already read')); this.bodyUsed = true; return Promise.resolve(this._b); }
    text() { return this._take().then((b) => td.decode(b)); }
    json() { return this.text().then((t) => JSON.parse(t)); }
    arrayBuffer() { return this._take().then((b) => b.slice().buffer); }
    bytes() { return this._take().then((b) => b.slice()); }
    blob() { return this._take().then((b) => new Blob([b], { type: this.headers.get('content-type') || '' })); }
    formData() { return this.text().then((t) => { const f = new FormData(); for (const [k, v] of new URLSearchParams(t)) f.append(k, v); return f; }); }
    clone() { if (this.bodyUsed) throw new TypeError('Response body is already used'); return new Response(this._b.slice(), { status: this.status, statusText: this.statusText, headers: this.headers, url: this.url }); }
    get body() { return null; }
    static error() { const r = new Response(null, { status: 0 }); r.type = 'error'; return r; }
    static json(data, init = {}) { const h = new Headers(init.headers); h.set('content-type', 'application/json'); return new Response(JSON.stringify(data), Object.assign({}, init, { headers: h })); }
    static redirect(url, status = 302) { return new Response(null, { status, headers: { location: new URL(url, document.baseURI).href } }); }
}
class Request {
    constructor(input, init = {}) {
        if (input instanceof Request) { init = Object.assign({ method: input.method, headers: input.headers, body: input._body, signal: input.signal }, init); input = input.url; }
        this.url = new URL(String(input), document.baseURI).href;
        this.method = String(init.method || 'GET').toUpperCase();
        this.headers = new Headers(init.headers);
        hidden(this, '_body', init.body === undefined ? null : init.body);
        this.signal = init.signal || new AbortController().signal;
        this.credentials = init.credentials || 'same-origin';
        this.mode = init.mode || 'cors';
        this.cache = init.cache || 'default';
        this.redirect = init.redirect || 'follow';
        this.referrer = 'about:client';
        this.integrity = init.integrity || '';
        this.keepalive = !!init.keepalive;
    }
    clone() { return new Request(this); }
    text() { return Promise.resolve(this._body === null ? '' : String(this._body)); }
    json() { return this.text().then(JSON.parse); }
}
hidden(G, 'Response', Response);
hidden(G, 'Request', Request);
function headerText(h) { let s = ''; for (const [k, v] of h._h) if (k !== 'content-type' && !/[\r\n]/.test(k + v)) s += k + ': ' + v + '\r\n'; return s; }
function fetch_(input, init) {
    return new Promise((resolve, reject) => {
        let req;
        try { req = new Request(input, init); } catch (e) { reject(e instanceof TypeError ? e : new TypeError(e.message)); return; }
        const sig = req.signal;
        if (sig.aborted) { reject(sig.reason); return; }
        const u = req.url;
        if (u.startsWith('blob:')) {
            const b = blobURLs.get(u);
            if (b) resolve(new Response(b._b.slice(), { status: 200, url: u, headers: { 'content-type': b.type } }));
            else reject(new TypeError('Failed to fetch'));
            return;
        }
        if (u.startsWith('data:')) {
            const m = /^data:([^,]*?)(;base64)?,(.*)$/s.exec(u);
            if (!m) { reject(new TypeError('Failed to fetch')); return; }
            let bytes;
            try { bytes = m[2] ? Uint8Array.from(atob(m[3]), (c) => c.charCodeAt(0)) : te.encode(decodeURIComponent(m[3])); } catch (e) { reject(new TypeError('Failed to fetch')); return; }
            resolve(new Response(bytes, { status: 200, url: u, headers: { 'content-type': m[1] || 'text/plain;charset=US-ASCII' } }));
            return;
        }
        const body = req.method === 'GET' || req.method === 'HEAD' ? null : bodyBytes(req._body, req.headers);
        let done = false;
        const id = N.fetch(u, req.method, headerText(req.headers), req.headers.get('content-type') || '', body, (status, statusText, url, hdrs, bytes, err) => {
            if (done) return;
            done = true;
            if (err) { reject(new TypeError('Failed to fetch' + (err ? ' (' + err + ')' : ''))); return; }
            const h = new Headers();
            for (let i = 0; i + 1 < hdrs.length; i += 2) h.append(hdrs[i], hdrs[i + 1]);
            resolve(new Response(bytes, { status, statusText, headers: h, url, redirected: url !== u }));
        });
        sig.addEventListener('abort', () => { if (!done) { done = true; N.abort(id); reject(sig.reason); } });
    });
}
hidden(G, 'fetch', fetch_);

class XMLHttpRequestEventTarget extends EventTarget {}
const XHR_EVENTS = ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend'];
class XMLHttpRequest extends XMLHttpRequestEventTarget {
    constructor() {
        super();
        this.readyState = 0; this.status = 0; this.statusText = ''; this.responseURL = ''; this.response = null;
        this.responseType = ''; this.timeout = 0; this.withCredentials = false; this.onreadystatechange = null;
        for (const e of XHR_EVENTS) this['on' + e] = null;
        this.upload = new XMLHttpRequestEventTarget();
        hidden(this, '_x', { id: 0, headers: new Headers(), rh: [], text: '', bytes: null, mime: null, timer: 0 });
    }
    get responseText() {
        if (this.responseType !== '' && this.responseType !== 'text') throw new DOMException("Failed to read the 'responseText' property from 'XMLHttpRequest': The value is only accessible if the object's 'responseType' is '' or 'text'.", 'InvalidStateError');
        return this._x.text;
    }
    get responseXML() { return null; }
    _fire(t, ev) {
        const e = ev || (t === 'readystatechange' ? new Event(t) : new ProgressEvent(t, { loaded: this._x.bytes ? this._x.bytes.length : 0, total: this._x.bytes ? this._x.bytes.length : 0, lengthComputable: !!this._x.bytes }));
        const h = this['on' + t];
        if (typeof h === 'function') { try { h.call(this, e); } catch (x) { report(x); } }
        this.dispatchEvent(e);
    }
    _state(s) { this.readyState = s; this._fire('readystatechange'); }
    open(method, url, async = true, user, pw) {
        if (async === false) throw new DOMException("Synchronous XMLHttpRequest is not supported in NovaOS: the page would stop responding.", 'InvalidAccessError');
        this.abort();
        const x = this._x;
        x.method = String(method).toUpperCase();
        x.url = new URL(String(url), document.baseURI).href;
        x.headers = new Headers();
        x.rh = []; x.text = ''; x.bytes = null; x.sent = false; x.aborted = false;
        this.status = 0; this.statusText = ''; this.response = null; this.responseURL = '';
        this._state(1);
    }
    setRequestHeader(k, v) { if (this.readyState !== 1 || this._x.sent) throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError'); this._x.headers.append(k, v); }
    overrideMimeType(m) { this._x.mime = String(m); }
    getResponseHeader(k) { k = String(k).toLowerCase(); const v = this._x.rh.filter((h) => h[0] === k).map((h) => h[1]); return this.readyState < 2 || !v.length ? null : v.join(', '); }
    getAllResponseHeaders() { return this.readyState < 2 ? '' : this._x.rh.map((h) => h[0] + ': ' + h[1]).join('\r\n') + (this._x.rh.length ? '\r\n' : ''); }
    send(body) {
        const x = this._x;
        if (this.readyState !== 1 || x.sent) throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
        x.sent = true;
        const bytes = x.method === 'GET' || x.method === 'HEAD' ? null : bodyBytes(body, x.headers);
        this._fire('loadstart');
        if (x.url.startsWith('data:') || x.url.startsWith('blob:')) {
            fetch_(x.url).then((r) => r.arrayBuffer().then((ab) => this._done(r.status, '', r.url, Array.from(r.headers.entries()).flat(), new Uint8Array(ab), ''))).catch(() => this._done(0, '', x.url, [], null, 'error'));
            return;
        }
        x.id = N.fetch(x.url, x.method, headerText(x.headers), x.headers.get('content-type') || '', bytes, (st, stt, url, hdrs, data, err) => this._done(st, stt, url, hdrs, data, err));
        if (this.timeout > 0) x.timer = setTimeout(() => { if (this.readyState !== 4) { N.abort(x.id); x.aborted = true; this.readyState = 4; this._fire('readystatechange'); this._fire('timeout'); this._fire('loadend'); } }, this.timeout);
    }
    _done(st, stt, url, hdrs, data, err) {
        const x = this._x;
        if (x.aborted) return;
        clearTimeout(x.timer);
        if (err) { this.status = 0; this.readyState = 4; this._fire('readystatechange'); this._fire('error'); this._fire('loadend'); return; }
        x.rh = [];
        for (let i = 0; i + 1 < hdrs.length; i += 2) x.rh.push([String(hdrs[i]).toLowerCase(), hdrs[i + 1]]);
        this.status = st; this.statusText = stt; this.responseURL = url;
        x.bytes = data || new Uint8Array(0);
        this._state(2);
        x.text = td.decode(x.bytes);
        const t = this.responseType;
        if (t === '' || t === 'text') this.response = x.text;
        else if (t === 'json') { try { this.response = JSON.parse(x.text); } catch (e) { this.response = null; } }
        else if (t === 'arraybuffer') this.response = x.bytes.slice().buffer;
        else if (t === 'blob') this.response = new Blob([x.bytes], { type: this.getResponseHeader('content-type') || '' });
        else if (t === 'document') { try { this.response = new DOMParser().parseFromString(x.text, 'text/html'); } catch (e) { this.response = null; } }
        this._state(3);
        this._fire('progress');
        this.readyState = 4;
        this._fire('readystatechange');
        this._fire('load');
        this._fire('loadend');
    }
    abort() {
        const x = this._x;
        if (x.id && this.readyState > 0 && this.readyState < 4 && x.sent) { N.abort(x.id); x.aborted = true; this.readyState = 4; this._fire('readystatechange'); this._fire('abort'); this._fire('loadend'); }
        this.readyState = 0;
    }
}
Object.assign(XMLHttpRequest, { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 });
hidden(G, 'XMLHttpRequest', XMLHttpRequest);
hidden(G, 'XMLHttpRequestEventTarget', XMLHttpRequestEventTarget);
class WebSocket extends EventTarget {
    constructor(url) {
        super();
        this.url = String(url); this.readyState = 0; this.protocol = ''; this.extensions = ''; this.bufferedAmount = 0; this.binaryType = 'blob';
        this.onopen = this.onclose = this.onerror = this.onmessage = null;
        setTimeout(() => {
            this.readyState = 3;
            const e = new Event('error');
            if (typeof this.onerror === 'function') this.onerror(e);
            this.dispatchEvent(e);
            const c = new Event('close');
            c.code = 1006; c.reason = 'WebSockets are not supported in NovaOS'; c.wasClean = false;
            if (typeof this.onclose === 'function') this.onclose(c);
            this.dispatchEvent(c);
        }, 0);
    }
    send() { throw new DOMException("Failed to execute 'send' on 'WebSocket': Still in CONNECTING state.", 'InvalidStateError'); }
    close() { this.readyState = 3; }
}
Object.assign(WebSocket, { CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 });
hidden(G, 'WebSocket', WebSocket);
class MessagePort extends EventTarget {
    constructor() { super(); this.onmessage = null; hidden(this, '_other', null); hidden(this, '_closed', false); }
    postMessage(data) {
        const o = this._other;
        if (!o || this._closed) return;
        let d;
        try { d = structuredClone(data); } catch (e) { d = data; }
        setTimeout(() => {
            if (o._closed) return;
            const e = new MessageEvent('message', { data: d });
            if (typeof o.onmessage === 'function') { try { o.onmessage.call(o, e); } catch (x) { report(x); } }
            o.dispatchEvent(e);
        }, 0);
    }
    start() {}
    close() { hidden(this, '_closed', true); }
}
class MessageChannel {
    constructor() { this.port1 = new MessagePort(); this.port2 = new MessagePort(); hidden(this.port1, '_other', this.port2); hidden(this.port2, '_other', this.port1); }
}
hidden(G, 'MessagePort', MessagePort);
hidden(G, 'MessageChannel', MessageChannel);
class BroadcastChannel extends EventTarget { constructor(n) { super(); this.name = String(n); this.onmessage = null; } postMessage() {} close() {} }
hidden(G, 'BroadcastChannel', BroadcastChannel);

/* ---- observers ------------------------------------------------------------------------------ */
const observers = [];
class MutationObserver {
    constructor(cb) { if (typeof cb !== 'function') throw new TypeError("Failed to construct 'MutationObserver': The callback provided as parameter 1 is not a function."); hidden(this, '_cb', cb); hidden(this, '_t', []); hidden(this, '_q', []); }
    observe(target, opts = {}) {
        if (!(target instanceof Node)) throw new TypeError("Failed to execute 'observe' on 'MutationObserver': parameter 1 is not of type 'Node'.");
        const o = {
            childList: !!opts.childList, subtree: !!opts.subtree,
            attributes: opts.attributes !== undefined ? !!opts.attributes : !!(opts.attributeFilter || opts.attributeOldValue),
            characterData: opts.characterData !== undefined ? !!opts.characterData : !!opts.characterDataOldValue,
            attributeFilter: opts.attributeFilter ? Array.from(opts.attributeFilter, String) : null,
            attributeOldValue: !!opts.attributeOldValue, characterDataOldValue: !!opts.characterDataOldValue,
        };
        const i = this._t.findIndex((x) => x.node === target);
        if (i >= 0) this._t[i].o = o; else this._t.push({ node: target, o });
        if (!observers.includes(this)) observers.push(this);
        N.observe(true);
    }
    disconnect() { this._t.length = 0; this._q.length = 0; const i = observers.indexOf(this); if (i >= 0) observers.splice(i, 1); N.observe(observers.length > 0); }
    takeRecords() { const q = this._q.slice(); this._q.length = 0; return q; }
}
hidden(G, 'MutationObserver', MutationObserver);
hidden(G, 'WebKitMutationObserver', MutationObserver);
hidden(G, 'MutationRecord', function MutationRecord() {});
let moPending = false;
/* called by the natives for every change while observers exist */
function mutation(type, target, a, b, c, d, e) {
    for (const mo of observers) {
        for (const { node, o } of mo._t) {
            if (node !== target && !(o.subtree && N.contains(node, target))) continue;
            let rec = null;
            if (type === 0 && o.childList) rec = { type: 'childList', target, addedNodes: staticList(a || []), removedNodes: staticList(b || []), previousSibling: c || null, nextSibling: d || null, attributeName: null, attributeNamespace: null, oldValue: null };
            else if (type === 1 && o.attributes && (!o.attributeFilter || o.attributeFilter.includes(a))) rec = { type: 'attributes', target, attributeName: a, attributeNamespace: null, oldValue: o.attributeOldValue ? b : null, addedNodes: staticList([]), removedNodes: staticList([]), previousSibling: null, nextSibling: null };
            else if (type === 2 && o.characterData) rec = { type: 'characterData', target, oldValue: o.characterDataOldValue ? a : null, attributeName: null, attributeNamespace: null, addedNodes: staticList([]), removedNodes: staticList([]), previousSibling: null, nextSibling: null };
            if (rec) { mo._q.push(rec); break; }
        }
    }
    if (!moPending && observers.some((m) => m._q.length)) {
        moPending = true;
        queueMicrotask(() => {
            moPending = false;
            for (const mo of observers.slice()) {
                const q = mo.takeRecords();
                if (q.length) { try { mo._cb.call(mo, q, mo); } catch (x) { report(x); } }
            }
        });
    }
}
class IntersectionObserver {
    constructor(cb, opts = {}) { hidden(this, '_cb', cb); hidden(this, '_t', []); this.root = opts.root || null; this.rootMargin = opts.rootMargin || '0px'; this.thresholds = [].concat(opts.threshold || 0); }
    observe(el) {
        if (this._t.includes(el)) return;
        this._t.push(el);
        /* everything counts as visible: lazy loaders show their content at once */
        setTimeout(() => {
            if (!this._t.includes(el)) return;
            const r = el.getBoundingClientRect();
            const entry = { target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r,
                rootBounds: new DOMRect(0, 0, G.innerWidth, G.innerHeight), time: N.now(), isVisible: true };
            try { this._cb([entry], this); } catch (x) { report(x); }
        }, 0);
    }
    unobserve(el) { const i = this._t.indexOf(el); if (i >= 0) this._t.splice(i, 1); }
    disconnect() { this._t.length = 0; }
    takeRecords() { return []; }
}
hidden(G, 'IntersectionObserver', IntersectionObserver);
hidden(G, 'IntersectionObserverEntry', function IntersectionObserverEntry() {});
class ResizeObserver {
    constructor(cb) { hidden(this, '_cb', cb); hidden(this, '_t', []); }
    observe(el) {
        if (this._t.includes(el)) return;
        this._t.push(el);
        requestAnimationFrame(() => {
            if (!this._t.includes(el)) return;
            const r = el.getBoundingClientRect();
            const size = [{ inlineSize: r.width, blockSize: r.height }];
            try { this._cb([{ target: el, contentRect: r, borderBoxSize: size, contentBoxSize: size, devicePixelContentBoxSize: size }], this); } catch (x) { report(x); }
        });
    }
    unobserve(el) { const i = this._t.indexOf(el); if (i >= 0) this._t.splice(i, 1); }
    disconnect() { this._t.length = 0; }
}
hidden(G, 'ResizeObserver', ResizeObserver);

/* ---- small constructors ---------------------------------------------------------------------- */
function Image(w, h) { const i = document.createElement('img'); if (w !== undefined) i.width = w; if (h !== undefined) i.height = h; return i; }
Image.prototype = I('Image').prototype;
hidden(G, 'Image', Image);
function Option(text = '', value, defSel, sel) {
    const o = document.createElement('option');
    if (text !== '') o.text = text;
    if (value !== undefined) o.value = value;
    if (defSel) o.defaultSelected = true;
    if (sel) o.selected = true;
    return o;
}
Option.prototype = optionP;
hidden(G, 'Option', Option);
function Audio(src) { const a = document.createElement('audio'); if (src !== undefined) a.src = src; return a; }
Audio.prototype = I('Audio').prototype;
hidden(G, 'Audio', Audio);
hidden(G, 'speechSynthesis', undefined);
delete G.speechSynthesis;

/* ---- input: focus, activation, pointer and keys ------------------------------------------- */
const mk = (...a) => hooks.makeEvent(...a);
const isText = (el) => !!el && el.nodeType === 1 && ((el.localName === 'input' && !/^(checkbox|radio|submit|reset|button|image|hidden|file|range|color)$/.test(el.type)) || el.localName === 'textarea');
const isCtl = (el) => !!el && el.nodeType === 1 && /^(input|button|select|textarea)$/.test(el.localName);
function focusable(el) {
    if (!el || el.nodeType !== 1 || !el.isConnected) return false;
    if (isCtl(el)) return !el.hasAttribute('disabled') && !(el.localName === 'input' && el.type === 'hidden');
    if ((el.localName === 'a' || el.localName === 'area') && el.hasAttribute('href')) return true;
    return el.hasAttribute('tabindex') || el.localName === 'summary' || el.getAttribute('contenteditable') === 'true';
}
let focusVal = null, pressTarget = null, hoverTarget = null, keyBlocked = false;
/* a text field's value is committed (change) when it loses focus or on Enter */
function commit(el) {
    if (isText(el) && focusVal !== null && el.value !== focusVal) {
        focusVal = el.value;
        dispatch(el, mk(0, 'change', true, false));
    }
}
function focusEl(el) {
    const old = N.active();
    if (old === el) return;
    if (old) {
        commit(old);
        N.ctl(old, 7);
        dispatch(old, mk(3, 'blur', false, false, el));
        dispatch(old, mk(3, 'focusout', true, false, el));
    }
    focusVal = null;
    if (!el) return;
    N.ctl(el, 6);
    focusVal = isText(el) ? el.value : null;
    dispatch(el, mk(3, 'focus', false, false, old));
    dispatch(el, mk(3, 'focusin', true, false, old));
}
function common(a, b) {
    if (!a || !b) return null;
    for (let n = a; n; n = n.parentNode) if (n === b || N.contains(n, b)) return n;
    return null;
}
const activatable = (n) => {
    const t = n.localName;
    if (t === 'input') return !/^(hidden|text|search|email|url|tel|password|number|date|time|datetime-local|month|week)$/.test(n.type) || n.type === 'image';
    return t === 'button' || t === 'select' || t === 'label' || t === 'summary' || ((t === 'a' || t === 'area') && n.hasAttribute('href'));
};
function submitForm(form, submitter, events) {
    if (!form || !form.isConnected) return;
    if (events) {
        const novalidate = form.hasAttribute('novalidate') || (submitter && submitter.hasAttribute('formnovalidate'));
        if (!novalidate) {
            const bad = Array.from(N.ctl(form, 21)).find((e) => e.checkValidity && !validity(e).valid);
            if (bad) {
                bad.dispatchEvent(new Event('invalid', { cancelable: true }));
                focusEl(bad);
                N.status(bad.validationMessage || 'Please fill out this field.');
                return;
            }
        }
        const ev = mk(7, 'submit', true, true, submitter);
        dispatch(form, ev);
        if (ev.defaultPrevented) return;
    }
    N.ctl(form, 9, submitter);
}
/* a click on t (a user's, el.click() or a key): the event, then what the element does */
function activate(t, cx, cy, mods, synthetic) {
    let act = null;
    for (let n = t; n && n.nodeType === 1; n = n.parentNode) if (activatable(n)) { act = n; break; }
    if (act && act.localName !== 'select' && isCtl(act) && act.hasAttribute('disabled')) return;
    let undo = null;
    if (act && act.localName === 'input' && (act.type === 'checkbox' || act.type === 'radio')) {
        const was = act.checked;
        let prev = null;
        if (act.type === 'radio' && act.name) prev = Array.from(document.querySelectorAll('input[type=radio]')).find((r) => r !== act && r.name === act.name && r.form === act.form && r.checked) || null;
        act.checked = act.type === 'checkbox' ? !was : true;
        undo = () => { act.checked = was; if (prev) prev.checked = true; };
        if (act.type === 'radio' && was) undo = null;
    }
    const ev = mk(1, 'click', true, true, cx, cy, 0, 0, mods);
    if (synthetic) ev.isTrusted = false;
    dispatch(t, ev);
    if (ev.defaultPrevented) { if (undo) undo(); return; }
    if (!act) return;
    const tag = act.localName;
    if (tag === 'input' && (act.type === 'checkbox' || act.type === 'radio')) {
        if (undo || act.type === 'checkbox') {
            dispatch(act, mk(4, 'input', true, false, null, ''));
            dispatch(act, mk(0, 'change', true, false));
        }
    } else if ((tag === 'input' && (act.type === 'submit' || act.type === 'image')) || (tag === 'button' && act.type === 'submit')) {
        submitForm(act.form, act, true);
    } else if ((tag === 'input' || tag === 'button') && act.type === 'reset') {
        if (act.form) act.form.reset();
    } else if (tag === 'select') {
        if (!synthetic) N.ctl(act, 30);
    } else if (tag === 'label') {
        const c = act.control;
        if (c && c !== t && !N.contains(c, t)) {
            if (isText(c) || c.localName === 'select') focusEl(c);
            else activate(c, cx, cy, mods, true);
        }
    } else if (tag === 'summary') {
        const d = act.parentNode;
        if (d && d.localName === 'details') { d.open = !d.open; setTimeout(() => dispatch(d, mk(0, 'toggle', false, false)), 0); }
    } else if ((tag === 'a' || tag === 'area') && synthetic) {
        follow(act);
    }
}
function follow(a) {
    const href = (a.getAttribute('href') || '').trim();
    if (/^javascript:/i.test(href)) { let code = href.slice(11); try { code = decodeURIComponent(code); } catch (e) { /* ignore */ } hooks.jsurl(code); return; }
    N.follow(a);
}
const hasHover = () => N.listening(0);
function pointer(type, t, cx, cy, button, buttons, mods) {
    switch (type) {
    case 0: {                                       /* button down */
        pressTarget = t;
        if (N.listening(1)) dispatch(t, mk(5, 'pointerdown', true, true, cx, cy, button, buttons, mods));
        const ev = mk(1, 'mousedown', true, true, cx, cy, button, buttons, mods);
        dispatch(t, ev);
        if (!ev.defaultPrevented && button === 0) {
            let f = t;
            while (f && f.nodeType === 1 && !focusable(f)) f = f.parentNode;
            if (!f || f.nodeType !== 1) f = null;
            if (f !== N.active()) focusEl(f);
            if (f && isText(f)) N.ctl(f, 25, cx, cy);
        }
        return !ev.defaultPrevented;
    }
    case 1: {                                       /* button up */
        if (N.listening(1)) dispatch(t, mk(5, 'pointerup', true, true, cx, cy, button, buttons, mods));
        dispatch(t, mk(1, 'mouseup', true, true, cx, cy, button, buttons, mods));
        if (button === 0) {
            const ct = common(pressTarget, t);
            pressTarget = null;
            /* (links: the browser follows them and sends the click with them) */
            if (ct && !N.inLink(ct)) activate(ct, cx, cy, mods, false);
        } else if (button === 1) {
            dispatch(t, mk(1, 'auxclick', true, true, cx, cy, button, buttons, mods));
        }
        return true;
    }
    case 2: {                                       /* move */
        if (t !== hoverTarget) {
            const old = hoverTarget;
            hoverTarget = t;
            if (old && old.isConnected) {
                dispatch(old, mk(1, 'mouseout', true, true, cx, cy, 0, buttons, mods));
                for (let n = old; n && n.nodeType === 1 && !(t && N.contains(n, t)); n = n.parentNode) dispatch(n, mk(1, 'mouseleave', false, false, cx, cy, 0, buttons, mods));
            }
            if (t) {
                dispatch(t, mk(1, 'mouseover', true, true, cx, cy, 0, buttons, mods));
                const enter = [];
                for (let n = t; n && n.nodeType === 1 && !(old && N.contains(n, old)); n = n.parentNode) enter.unshift(n);
                for (const n of enter) dispatch(n, mk(1, 'mouseenter', false, false, cx, cy, 0, buttons, mods));
            }
        }
        if (t) {
            if (N.listening(1)) dispatch(t, mk(5, 'pointermove', true, true, cx, cy, 0, buttons, mods));
            dispatch(t, mk(1, 'mousemove', true, true, cx, cy, 0, buttons, mods));
        }
        return true;
    }
    case 3:                                         /* double click */
        dispatch(t, mk(1, 'dblclick', true, true, cx, cy, 0, 0, mods));
        return true;
    case 4:                                         /* the pointer left the page */
        if (hoverTarget && hoverTarget.isConnected) {
            dispatch(hoverTarget, mk(1, 'mouseout', true, true, cx, cy, 0, 0, mods));
            for (let n = hoverTarget; n && n.nodeType === 1; n = n.parentNode) dispatch(n, mk(1, 'mouseleave', false, false, cx, cy, 0, 0, mods));
        }
        hoverTarget = null;
        return true;
    case 5: {                                       /* wheel (buttons: notches) */
        const ev = mk(6, 'wheel', true, true, cx, cy, 0, 0, mods, buttons * 100);
        dispatch(t, ev);
        return !ev.defaultPrevented;
    }
    }
    return true;
}
/* keys: 0 down, 1 a character, 2 up. true = the page used it (no scrolling) */
function key(type, k, repeat, mods, code, rawmods) {
    const t = N.active() || document.body || document.documentElement;
    if (!t) return false;
    if (type === 2) { dispatch(t, mk(2, 'keyup', true, true, k, repeat, 0, 0, mods)); return false; }
    if (type === 1) {
        if (keyBlocked) return true;
        const ev = mk(2, 'keypress', true, true, k, repeat, 0, 0, mods);
        dispatch(t, ev);
        if (ev.defaultPrevented || !isText(t) || t.readOnly || t.disabled) return isText(t);
        if (N.ctl(t, 26, k)) dispatch(t, mk(4, 'input', true, false, k, 'insertText'));
        return true;
    }
    const ev = mk(2, 'keydown', true, true, k, repeat, 0, 0, mods);
    dispatch(t, ev);
    keyBlocked = ev.defaultPrevented;
    if (keyBlocked) return true;
    if (k === 'Enter') {                            /* (Enter makes a character: keypress too) */
        const kp = mk(2, 'keypress', true, true, k, repeat, 0, 0, mods);
        dispatch(t, kp);
        if (kp.defaultPrevented) return true;
        if (N.active() !== t && t !== document.body) return true;
    }
    if (k === 'Tab') {
        const all = Array.from(document.querySelectorAll('input,select,textarea,button,a[href],[tabindex]')).filter((e) => focusable(e) && e.tabIndex >= 0 && N.rect(e));
        if (!all.length) return false;
        const i = all.indexOf(t);
        focusEl(all[(i + ((mods & 2) ? all.length - 1 : 1) + (i < 0 ? 1 : 0)) % all.length]);
        return true;
    }
    if (isText(t)) {
        if (k === 'Enter' && t.localName !== 'textarea') {
            commit(t);
            const f = t.form;
            if (f) {
                const b = Array.from(N.ctl(f, 21)).find((e) => (e.localName === 'button' && e.type === 'submit') || (e.localName === 'input' && (e.type === 'submit' || e.type === 'image')));
                if (b) { if (!b.disabled) activate(b, 0, 0, mods, true); } else submitForm(f, null, true);
            }
            return true;
        }
        if (k === 'Escape') { focusEl(null); return true; }
        const r = N.key(t, code, rawmods);
        if (r === 2) dispatch(t, mk(4, 'input', true, false, null, k === 'Delete' ? 'deleteContentForward' : 'deleteContentBackward'));
        return r !== 0 || k === ' ' || /^Arrow(Up|Down)$/.test(k);
    }
    if ((k === ' ' || k === 'Enter') && t !== document.body && t.nodeType === 1) {
        const tag = t.localName;
        if ((k === ' ' && (tag === 'button' || (tag === 'input' && !isText(t)) || tag === 'summary')) || (k === 'Enter' && (tag === 'button' || tag === 'summary' || ((tag === 'a' || tag === 'area') && t.hasAttribute('href'))))) {
            activate(t, 0, 0, mods, true);
            return true;
        }
        if (tag === 'select' && k === ' ') { N.ctl(t, 30); return true; }
    }
    if (k === 'Escape' && t !== document.body) { focusEl(null); return true; }
    return false;
}

/* ---- the hooks the browser calls ------------------------------------------------------------ */
const keyCodes = { Enter: 13, Tab: 9, Escape: 27, Backspace: 8, Delete: 46, ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39,
    ArrowDown: 40, Home: 36, End: 35, PageUp: 33, PageDown: 34, ' ': 32, Shift: 16, Control: 17, Alt: 18, Insert: 45,
    F1: 112, F2: 113, F3: 114, F4: 115, F5: 116, F6: 117, F7: 118, F8: 119, F9: 120, F10: 121, F11: 122, F12: 123 };
const codeOf = (k) => {
    if (/^[a-z]$/i.test(k)) return 'Key' + k.toUpperCase();
    if (/^[0-9]$/.test(k)) return 'Digit' + k;
    if (k === ' ') return 'Space';
    return { Enter: 'Enter', Tab: 'Tab', Escape: 'Escape', Backspace: 'Backspace', Delete: 'Delete', ArrowLeft: 'ArrowLeft',
        ArrowUp: 'ArrowUp', ArrowRight: 'ArrowRight', ArrowDown: 'ArrowDown', Home: 'Home', End: 'End', PageUp: 'PageUp',
        PageDown: 'PageDown', '-': 'Minus', '=': 'Equal', ',': 'Comma', '.': 'Period', '/': 'Slash', ';': 'Semicolon',
        "'": 'Quote', '[': 'BracketLeft', ']': 'BracketRight', '\\': 'Backslash', '`': 'Backquote' }[k] || k;
};
const hooks = {
    dispatch,
    pointer,
    key,
    /* the browser follows a link the user clicked: the click first (target: what was under the pointer) */
    link(t, a, cx, cy, mods) {
        const ev = mk(1, 'click', true, true, cx, cy, 0, 0, mods);
        dispatch(t || a, ev);
        if (!ev.defaultPrevented) follow(a);
    },
    /* a choice from a <select>'s list */
    selected(el) { dispatch(el, mk(4, 'input', true, false, null, '')); dispatch(el, mk(0, 'change', true, false)); },
    fire(t, type, bubbles, cancelable) { const e = mk(0, type, bubbles, cancelable); dispatch(t, e); return !e.defaultPrevented; },
    event(t, kind, type, bubbles, cancelable, a, b) { const e = mk(kind, type, bubbles, cancelable, a, b); dispatch(t, e); return !e.defaultPrevented; },
    /* kind 0 Event, 1 MouseEvent, 2 KeyboardEvent, 3 FocusEvent, 4 InputEvent, 5 PointerEvent, 6 WheelEvent,
       7 SubmitEvent, 8 HashChangeEvent, 9 PopStateEvent, 10 PageTransitionEvent, 11 ErrorEvent */
    makeEvent(kind, type, bubbles, cancelable, a, b, c, d, mods, extra) {
        const i = { bubbles, cancelable, view: G };
        if (mods) { i.ctrlKey = !!(mods & 1); i.shiftKey = !!(mods & 2); i.altKey = !!(mods & 4); }
        let e;
        switch (kind) {
        case 1: case 5: case 6:
            i.clientX = a; i.clientY = b; i.button = c; i.buttons = d; i.detail = type === 'click' || type === 'mouseup' || type === 'mousedown' ? 1 : type === 'dblclick' ? 2 : 0;
            if (kind === 6) { i.deltaY = extra; }
            e = kind === 1 ? new MouseEvent(type, i) : kind === 5 ? new PointerEvent(type, i) : new WheelEvent(type, i);
            if (type === 'mouseover' || type === 'mouseout') i.relatedTarget = null;
            break;
        case 2:
            i.key = a; i.code = codeOf(a); i.keyCode = keyCodes[a] || (a.length === 1 ? a.toUpperCase().charCodeAt(0) : 0);
            if (type === 'keypress') { i.charCode = a.length === 1 ? a.charCodeAt(0) : a === 'Enter' ? 13 : 0; i.keyCode = i.charCode; }
            i.repeat = !!b;
            e = new KeyboardEvent(type, i);
            break;
        case 3: i.relatedTarget = a || null; e = new FocusEvent(type, i); break;
        case 4: i.data = a; i.inputType = b || 'insertText'; e = new InputEvent(type, i); break;
        case 7: i.submitter = a || null; e = new SubmitEvent(type, i); break;
        case 8: i.oldURL = a; i.newURL = b; e = new HashChangeEvent(type, i); break;
        case 9: i.state = states.has(a) ? states.get(a) : null; e = new PopStateEvent(type, i); break;
        case 10: e = new PageTransitionEvent(type, i); break;
        case 11: i.message = a; e = new ErrorEvent(type, i); break;
        default: e = new Event(type, i);
        }
        e.isTrusted = true;
        return e;
    },
    mutation,
    upgrade,
    report,
    /* a javascript: URL */
    jsurl(code) { try { (0, eval)(code); } catch (e) { report(e); } },
};
return hooks;
})
