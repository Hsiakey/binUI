/*!
 * binui-preview.js —— binUI 源码的**浏览器侧运行时**（issue 15 / M2）
 *
 * 源码已经是标准 HTML + 标准 CSS 子集，浏览器直接打开就能看到骨架；
 * 只有组件内部结构需要一次展开（标准 HTML 没有 include）。这个脚本只干这几件事：
 *
 *   1. 展开 `data-component="x.bum.html"`
 *      参数名与规则跟编译器**完全同一套**：宿主属性名（含 `data-` 前缀去掉的那份）
 *      → 片段里的 `{{name}}` / `{{name=默认}}`；`data-param="x"` 的元素取宿主
 *      `data-x`，没给就用宿主标签里的文本（消费一次）。片段顶层元素挂到宿主下，
 *      宿主**就是**组件根，所以宿主自己保持原样。
 *   2. 把片段的 `<style>` 并进 `<head>`（与编译器的"组件样式并入样式表"同序：页面在后、
 *      组件按展开顺序追加在后）。
 *   3. `data-binui-window` 的 `data-width/data-height/data-bg` 当窗口几何用
 *      （标准 HTML 里 `data-*` 是惰性的，得有人把它翻译成盒子的尺寸）。
 *   4. 注入一份与 binUI 对齐的基础样式：默认字体链、默认行高 1.35×、`box-sizing:border-box`、
 *      UA 默认（margin/padding/button 边框）清零 —— 这些是 §5 表里实测出来的三条硬差异。
 *
 * **不参与布局计算**：布局由浏览器 + 源码里的 CSS 决定；像素权威依旧是 `bin2d_shot`。
 * 无 JS 时页面退化到"宿主元素的形态"（默认文本 + `var()` 的 fallback），这是设计内行为。
 *
 * 用它需要 HTTP（`file://` 下 fetch 被拦），所以正常入口是 `binui dev`：
 *   python tools/binui_dev.py examples/appshow/main.html
 *
 * 组件独立视图（居中 / 1x~8x / 盒边界）同一份脚本，由 `binui dev` 的
 *   /__binui/c/components/titlebar.bum.html?zoom=4&grid=1
 * 路由触发（`data-binui-standalone`）。
 */
(function () {
  'use strict';

  if (window.__binuiPreview) return;      // 被重复引入时只跑一次

  var MAX_DEPTH = 8;                      // 与编译器的组件嵌套上限一致
  var ZOOMS = [1, 2, 4, 8];
  var SCRIPT = document.currentScript || null;
  var DEV = !!(SCRIPT && SCRIPT.hasAttribute('data-binui-dev'));
  var STANDALONE = !!(SCRIPT && SCRIPT.hasAttribute('data-binui-standalone'));

  // binUI 渲染器的默认字体链（binui_font_host.cpp）与默认字号/行高（DefaultFontSizeOf / 135%）
  var FONT_STACK = '"Segoe UI","Microsoft YaHei","Segoe UI Symbol","Segoe UI Emoji",sans-serif';
  var DEFAULT_FONT_SIZE = 14;
  var DEFAULT_LINE_HEIGHT = 1.35;

  var state = {
    ok: true, done: false, hosts: 0, expanded: 0, styles: 0, absShim: 0, borderNoFlow: 0,
    warnings: [], errors: [],
    components: []                        // 展开过的组件（root 相对路径，去重）
  };

  var srcCache = Object.create(null);     // 绝对 URL → Promise<{path,text}>
  var styleDone = Object.create(null);    // 组件样式去重键
  var knownTags = null;                   // 标准 HTML 标签名（其余按块级盒处理）

  // ---------------------------------------------------------------- 小工具
  function trim(s) { return s == null ? '' : String(s).trim(); }
  function warn(msg) {
    state.warnings.push(msg);
    if (window.console && console.warn) console.warn('[binui-preview] ' + msg);
  }
  function fail(msg) {
    state.errors.push(msg);
    state.ok = false;
    if (window.console && console.error) console.error('[binui-preview] ' + msg);
  }
  function escapeText(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }
  // 长度：纯数字补 px，其它（100%、content、auto）原样
  function toLen(v) {
    var s = trim(v);
    if (!s) return '';
    return /^-?\d+(\.\d+)?$/.test(s) ? s + 'px' : s;
  }

  // ---------------------------------------------------------------- 基础样式
  var BASE_CSS = [
    /* 默认字体/字号/行高：写在 html 上，作者的 body 规则照样盖得住（继承 vs 直接命中） */
    'html[data-binui-preview]{font-family:' + FONT_STACK + ';font-size:' + DEFAULT_FONT_SIZE +
      'px;line-height:' + DEFAULT_LINE_HEIGHT + '}',
    /* 页面在 binUI 里是"窗口内容"，body 的盒子/外边距不参与绘制 —— 清零免得 8px 偏移 */
    ':where([data-binui-preview] html,[data-binui-preview] body){margin:0;padding:0;background:none}',
    /* binUI 恒为 border-box 且忽略作者的 box-sizing（§5 实测），所以这条要压得住作者 */
    'html[data-binui-preview] *,html[data-binui-preview] *::before,' +
      'html[data-binui-preview] *::after{box-sizing:border-box}',
    /* UA 默认清零：零特异性（:where）= 作者任何规则都赢，只用来抹掉浏览器的默认外观 */
    ':where([data-binui-preview] div,[data-binui-preview] span,[data-binui-preview] p,' +
      '[data-binui-preview] h1,[data-binui-preview] h2,[data-binui-preview] h3,' +
      '[data-binui-preview] h4,[data-binui-preview] h5,[data-binui-preview] h6,' +
      '[data-binui-preview] ul,[data-binui-preview] ol,[data-binui-preview] li,' +
      '[data-binui-preview] figure,[data-binui-preview] blockquote,[data-binui-preview] pre)' +
      '{margin:0;padding:0}',
    ':where([data-binui-preview] ul,[data-binui-preview] ol,[data-binui-preview] li)' +
      '{list-style:none}',
    ':where([data-binui-preview] h1,[data-binui-preview] h2,[data-binui-preview] h3,' +
      '[data-binui-preview] h4,[data-binui-preview] h5,[data-binui-preview] h6,' +
      '[data-binui-preview] b,[data-binui-preview] strong)' +
      '{font-size:inherit;font-weight:inherit}',
    /* 表单控件：binUI 里 button/input 就是普通盒子（没有 UA 边框/内边距/字体） */
    ':where([data-binui-preview] button,[data-binui-preview] input,' +
      '[data-binui-preview] select,[data-binui-preview] textarea)' +
      '{font:inherit;color:inherit;background:none;border:0;border-radius:0;padding:0;margin:0;' +
      'appearance:none;-webkit-appearance:none;text-align:inherit;line-height:inherit}',
    ':where([data-binui-preview] button,[data-binui-preview] input[type=button],' +
      '[data-binui-preview] input[type=submit]){cursor:default;text-align:center}',
    /* flex 项的"自动最小尺寸"：CSS 里 flex 项默认 min-width/min-height = auto，
       内容撑不下时会被 min-content 顶大、兄弟项被 flex-shrink 压扁（实测 flexshow：标题栏
       45px 被压到 30、body 撑到 760 溢出窗口）。binUI 没有这套"自动最小尺寸"，
       flex:1 就是把剩余空间吃掉 —— 零特异性显式写 0，作者自己写的 min-width 照样赢。 */
    ':where([data-binui-preview] *){min-width:0;min-height:0}',
    /* binUI 没有 inline 排版：每个元素都是一个块级盒（只有 display:none / flex 两个开关）。
       <span>/<b>/<a> 这类行内标签在浏览器里要拉成块，否则并排的文字会挤成一行互相重叠 ——
       实测：panel 组件的三行内容全叠在同一行上。 */
    ':where([data-binui-preview] span,[data-binui-preview] a,[data-binui-preview] b,' +
      '[data-binui-preview] i,[data-binui-preview] em,[data-binui-preview] strong,' +
      '[data-binui-preview] small,[data-binui-preview] code,[data-binui-preview] label,' +
      '[data-binui-preview] mark,[data-binui-preview] time,[data-binui-preview] abbr,' +
      '[data-binui-preview] cite,[data-binui-preview] q,[data-binui-preview] u,' +
      '[data-binui-preview] s,[data-binui-preview] var,[data-binui-preview] kbd,' +
      '[data-binui-preview] samp,[data-binui-preview] dfn,[data-binui-preview] sub,' +
      '[data-binui-preview] sup){display:block}',
    /* 首次展开时组件样式是"后来才插进 head"的：CSS 过渡会把初始渲染动画一遍（先透明后上色），
       而 binUI 首帧就是终值。展开期间关掉过渡，两帧后恢复。 */
    'html[data-binui-preview-boot] *,html[data-binui-preview-boot] *::before,' +
      'html[data-binui-preview-boot] *::after{transition:none!important}',
    /* 未知标签（<titlebar>/<panel>/…）：浏览器认作 inline，binUI 认作块级盒 */
    /* 具体规则在 applyUnknownTags() 里按实际出现的标签名生成 */
  ].join('\n');

  function injectBase() {
    document.documentElement.setAttribute('data-binui-preview', '');
    document.documentElement.setAttribute('data-binui-preview-boot', '');
    if (document.getElementById('binui-preview-base')) return;
    var st = document.createElement('style');
    st.id = 'binui-preview-base';
    st.setAttribute('data-binui', 'preview');
    st.textContent = BASE_CSS;
    document.head.appendChild(st);
  }

  // ---------------------------------------------------------------- 组件源码
  function loadComponent(url) {
    if (srcCache[url]) return srcCache[url];
    var req = '/__binui/component?src=' + encodeURIComponent(url);
    srcCache[url] = fetch(req, { cache: 'no-store' })
      .then(function (r) {
        var ct = r.headers.get('Content-Type') || '';
        if (ct.indexOf('application/json') < 0) {
          throw new Error('预览服务没在跑（' + r.status + '）—— 用 binui dev 打开，别用 file://');
        }
        return r.json();
      })
      .then(function (j) {
        if (!j.ok) throw new Error(j.error || ('读不到组件 ' + url));
        if (state.components.indexOf(j.path) < 0) state.components.push(j.path);
        return j;
      });
    return srcCache[url];
  }

  // ---------------------------------------------------------------- 参数替换
  // {{name}} → 宿主同名属性；{{name=默认}} 没给就用默认。与编译器 SubstituteParams 同规则：
  // 参数值按**纯文本**注入（转义 & < >），键名小写。
  function substitute(src, params) {
    return src.replace(/\{\{([\s\S]*?)\}\}/g, function (m, inner) {
      inner = trim(inner);
      var eq = inner.indexOf('=');
      var key = (eq < 0 ? inner : inner.slice(0, eq)).trim().toLowerCase();
      var hasDefault = eq >= 0;
      var fallback = hasDefault ? inner.slice(eq + 1).trim() : '';
      if (Object.prototype.hasOwnProperty.call(params, key)) return escapeText(params[key]);
      if (hasDefault) return escapeText(fallback);
      warn('组件参数 "' + key + '" 未提供，已按空串替换');
      return '';
    });
  }

  // 宿主属性 → 参数表：data-<x> 去掉前缀算一份，原名也算一份（兼容旧 {{x}} 组件）
  function paramsOf(host) {
    var p = Object.create(null);
    for (var i = 0; i < host.attributes.length; i++) {
      var a = host.attributes[i];
      if (a.name === 'data-component') continue;
      if (a.name.indexOf('data-') === 0) p[a.name.slice(5)] = a.value;
      p[a.name] = a.value;
    }
    return p;
  }

  function directText(host) {
    var t = '';
    for (var i = 0; i < host.childNodes.length; i++) {
      if (host.childNodes[i].nodeType === 3) t += host.childNodes[i].nodeValue;
    }
    return trim(t);
  }
  function dropDirectText(host) {
    for (var i = host.childNodes.length - 1; i >= 0; i--) {
      if (host.childNodes[i].nodeType === 3) host.removeChild(host.childNodes[i]);
    }
  }

  // 片段里 img/video 的相对 src 按**组件文件所在目录**解析（编译器同样改写路径）
  function rebaseAssets(el, compUrl) {
    var list = el.querySelectorAll('img[src],video[src],source[src]');
    for (var i = 0; i < list.length; i++) {
      var s = list[i].getAttribute('src');
      if (!s || /^(?:[a-z]+:|\/|#|data:)/i.test(s)) continue;
      try { list[i].setAttribute('src', new URL(s, compUrl).href); } catch (e) { /* 原样留着 */ }
    }
  }

  function collectStyles(frag, url) {
    var styles = frag.querySelectorAll('style');
    for (var i = 0; i < styles.length; i++) {
      var key = url + '#' + i;
      if (styleDone[key]) continue;
      styleDone[key] = true;
      var st = document.createElement('style');
      st.setAttribute('data-binui', 'component');
      st.setAttribute('data-binui-src', url);
      st.textContent = styles[i].textContent;
      document.head.appendChild(st);
      state.styles++;
    }
  }

  // ---------------------------------------------------------------- 展开
  function expand(host, depth, chain) {
    if (host.hasAttribute('data-binui-expanded')) return Promise.resolve();
    if (depth > MAX_DEPTH) {
      warn('data-component 嵌套超过 ' + MAX_DEPTH + ' 层，疑似循环引用，已停止展开');
      return Promise.resolve();
    }
    var srcRel = trim(host.getAttribute('data-component'));
    var label = host.id ? '#' + host.id : '<' + host.localName + '>';
    if (!srcRel) { warn('data-component 是空值：' + label); return Promise.resolve(); }

    var url;
    try { url = new URL(srcRel, document.baseURI).href; }
    catch (e) { warn('data-component 解析不了：' + srcRel); return Promise.resolve(); }
    if (chain.indexOf(url) >= 0) { warn('组件循环引用：' + srcRel + '（已跳过）'); return Promise.resolve(); }

    return loadComponent(url).then(function (j) {
      var params = paramsOf(host);
      var frag = document.createElement('template');
      frag.innerHTML = substitute(j.text, params);
      var content = frag.content;

      collectStyles(content, url);

      // 片段顶层只取**元素**：顶层裸文本是"叶组件的默认文案"，编译期同样不画（宿主文本才是默认值）；
      // style/script/link/meta/title/template 归样式表或直接忽略，不能当盒子挂上去。
      var SKIP = { style: 1, script: 1, link: 1, meta: 1, title: 1, template: 1 };
      var top = [];
      for (var n = content.firstChild; n; n = n.nextSibling) {
        if (n.nodeType === 1 && !SKIP[n.localName]) top.push(n);
      }

      // data-param：宿主 data-<name> 优先，否则用宿主标签里的文本（只消费一次）
      var hostText = directText(host);
      var consumed = false;
      var paramEls = [];
      for (var t = 0; t < top.length; t++) {
        if (top[t].hasAttribute('data-param')) paramEls.push(top[t]);
        var sub = top[t].querySelectorAll ? top[t].querySelectorAll('[data-param]') : [];
        for (var s = 0; s < sub.length; s++) paramEls.push(sub[s]);
      }
      for (var q = 0; q < paramEls.length; q++) {
        var name = trim(paramEls[q].getAttribute('data-param')).toLowerCase();
        if (params[name]) {
          paramEls[q].textContent = params[name];
          consumed = true;
        } else if (!consumed && hostText) {
          paramEls[q].textContent = hostText;
          consumed = true;
        }
      }
      if (consumed) dropDirectText(host);   // 宿主文本被参数消费掉：留着会与组件内文字重复

      var compUrl = new URL('/file/' + j.path, document.baseURI).href;
      var holder = document.createDocumentFragment();
      for (var a = 0; a < top.length; a++) {
        rebaseAssets(top[a], compUrl);
        holder.appendChild(top[a]);
      }
      // 片段元素排在**宿主原有子元素之前**：扁平元素表里顺序是 <宿主>、组件元素、宿主的孩子，
      // 所以 panel 的标题在内容上方（appendChild 会把标题排到内容下面，实测错位）。
      if (holder.firstChild) host.insertBefore(holder, host.firstChild);

      host.setAttribute('data-binui-expanded', '');
      state.expanded++;

      // 嵌套：片段里还能再有 data-component（递归深度与编译器一致）
      var chain2 = chain.concat([url]);
      var nested = host.querySelectorAll('[data-component]');
      var jobs = Promise.resolve();
      Array.prototype.forEach.call(nested, function (el) {
        jobs = jobs.then(function () { return expand(el, depth + 1, chain2); });
      });
      return jobs;
    }).catch(function (e) {
      fail('data-component 内联 ' + label + '：' + e.message);
    });
  }

  function expandAll() {
    var hosts = document.querySelectorAll('[data-component]');
    var job = Promise.resolve();
    Array.prototype.forEach.call(hosts, function (h) {
      job = job.then(function () { return expand(h, 0, []); });
    });
    return job.then(finish);
  }

  // ---------------------------------------------------------------- 语义补齐
  // binUI 没有 CSS 的"包含块链"：position:absolute 一律相对**父元素**定位。
  // 浏览器要往上找最近的定位祖先，父元素是静态时就会跑出父盒（E2E 实测：标题栏三颗
  // 按钮的图标跑到了标题栏左上角）。这里给"有绝对定位子元素、自己却静态"的父级补
  // position:relative —— 对 binUI 是无操作，对浏览器是把语义拉回同一套。
  function shimAbsContainingBlock() {
    // 只在"源码自己的盒"里做：组件宿主 / 窗口包裹层，避开 dev 页自己搭的工具栏与舞台
    var scopes = document.querySelectorAll('[data-component],[data-binui-window]');
    var touched = 0;
    for (var s = 0; s < scopes.length; s++) {
      var all = scopes[s].querySelectorAll('*');
      for (var i = 0; i < all.length; i++) {
        var el = all[i];
        var cs = getComputedStyle(el);
        if (cs.position !== 'absolute' && cs.position !== 'fixed') continue;
        var p = el.parentElement;
        if (!p || p === document.body || p === document.documentElement) continue;
        if (getComputedStyle(p).position !== 'static') continue;
        p.style.position = 'relative';
        touched++;
      }
    }
    state.absShim = touched;
    return touched;
  }

  // ---------------------------------------------------------------- 语义补齐
  // 边框：binUI 的 border **画在内侧、不参与布局**（include/binui_format.h 里写的设计），
  // 而 CSS 的 border 要占内容空间 —— 带 padding 的盒子里绝对定位子元素实测差 4px，不带 padding 差 1px，
  // % 宽和 flex 主轴也跟着差。预览这里把 border 换成 inset 阴影，让浏览器按 binUI 的盒子规则算。
  // 实测：换成 inset 阴影后 appshow 差异 6.47%→5.71%，但另外 6 个页面反而变差（inset 的 AA 不如真边框），
  // 净收益≈0 —— 所以**默认不做**，只在 ?border=noflow 时启用（做对照实验用）。
  function borderNoFlow() {
    if (!/(?:^|[?&])border=noflow\b/.test(location.search)) return 0;
    var all = document.body.getElementsByTagName('*');
    var n = 0;
    for (var i = 0; i < all.length; i++) {
      var el = all[i], cs = getComputedStyle(el);
      var w = parseFloat(cs.borderTopWidth) || 0;
      if (!w) continue;
      var sh = [];
      ['Top', 'Right', 'Bottom', 'Left'].forEach(function (side) {
        var bw = parseFloat(cs['border' + side + 'Width']) || 0;
        if (bw > 0) sh.push('inset ' + (side === 'Top' ? '0 ' + bw : side === 'Bottom' ? '0 -' + bw
          : side === 'Left' ? bw + ' 0' : '-' + bw + ' 0') + ' 0 ' + cs['border' + side + 'Color']);
      });
      if (sh.length) el.style.boxShadow = sh.join(',') + (cs.boxShadow && cs.boxShadow !== 'none' ? ',' + cs.boxShadow : '');
      el.style.borderWidth = '0';
      n++;
    }
    state.borderNoFlow = n;
    return n;
  }

  // ---------------------------------------------------------------- 窗口 / 未知标签
  function applyWindows() {
    var wins = document.querySelectorAll('[data-binui-window]');
    for (var i = 0; i < wins.length; i++) {
      var el = wins[i];
      var w = toLen(el.getAttribute('data-width'));
      var h = toLen(el.getAttribute('data-height'));
      var bg = trim(el.getAttribute('data-bg'));
      if (w) el.style.width = w;
      if (h) el.style.height = h;
      if (bg) el.style.background = bg;
      if (!el.style.position) el.style.position = 'relative';
      el.setAttribute('data-binui-window-ready', '');
    }
  }

  // 标准 HTML 标签名：其余（<titlebar>/<panel>/<statusbar>…）binUI 当块级盒
  function standardTags() {
    if (knownTags) return knownTags;
    knownTags = Object.create(null);
    (
      'a abbr address area article aside audio b base bdi bdo blockquote body br button canvas ' +
      'caption cite code col colgroup data datalist dd del details dfn dialog div dl dt em embed ' +
      'fieldset figcaption figure footer form h1 h2 h3 h4 h5 h6 head header hgroup hr html i ' +
      'iframe img input ins kbd label legend li link main map mark menu meta meter nav noscript ' +
      'object ol optgroup option output p param picture pre progress q rp rt ruby s samp script ' +
      'section select slot small source span strong style sub summary sup svg table tbody td ' +
      'template textarea tfoot th thead time title tr track u ul var video wbr'
    ).split(' ').forEach(function (t) { knownTags[t] = true; });
    return knownTags;
  }

  function applyUnknownTags() {
    var known = standardTags();
    var seen = Object.create(null);
    var all = document.querySelectorAll('*');
    for (var i = 0; i < all.length; i++) {
      var t = all[i].localName;
      if (!known[t] && !seen[t]) { seen[t] = true; all[i].setAttribute('data-binui-unknown', ''); }
    }
    var names = Object.keys(seen);
    if (!names.length) return;
    var st = document.createElement('style');
    st.setAttribute('data-binui', 'preview-unknown');
    st.textContent = names.join(',') + '{display:block}';
    document.head.appendChild(st);
    state.unknownTags = names;
  }

  // ---------------------------------------------------------------- 调试
  // ?layout=1：把每个元素的盒按 JSON 写进 <pre id="binui-layout">（相对视口/窗口左上角），
  // 用来和 binUI 的 `draw_bench --dump-nodes` 逐元素比几何 —— 像素比说不出"哪条属性不对"，这个能。
  function dumpLayout() {
    if (!/(?:^|[?&])layout\b/.test(location.search)) return;
    var els = document.body.getElementsByTagName('*');
    var out = [];
    for (var i = 0; i < els.length; i++) {
      var el = els[i];
      if (el.closest('[data-binui-preview-ui]')) continue;
      var r = el.getBoundingClientRect(), cs = getComputedStyle(el);
      out.push({ id: el.id || ('#' + i), tag: el.localName, cls: String(el.className || ''),
                 x: +r.left.toFixed(2), y: +r.top.toFixed(2),
                 w: +r.width.toFixed(2), h: +r.height.toFixed(2),
                 disp: cs.display, flex: cs.flex, minH: cs.minHeight, hgt: cs.height,
                 box: cs.boxSizing });
    }
    var pre = document.createElement('pre');
    pre.id = 'binui-layout';
    pre.style.cssText = 'position:fixed;left:-9999px;top:0';
    pre.textContent = JSON.stringify(out);
    document.body.appendChild(pre);
  }

  function dumpRects() {
    if (!/(?:^|[?&])debug\b/.test(location.search)) return;
    setTimeout(drawRects, 600);   // 等过渡跑完，读到的是最终值
  }

  function drawRects() {
    var hosts = document.querySelectorAll('[data-component]');
    var lines = [];
    for (var i = 0; i < hosts.length && i < 6; i++) {
      var h = hosts[i], hr = h.getBoundingClientRect(), hs = getComputedStyle(h);
      lines.push('#' + i + ' <' + h.localName + '> disp=' + hs.display + ' rect=' +
        Math.round(hr.left) + ',' + Math.round(hr.top) + ' ' +
        Math.round(hr.width) + 'x' + Math.round(hr.height) +
        ' bg=' + hs.backgroundColor + ' radius=' + hs.borderRadius + ' ta=' + hs.textAlign);
      for (var k = 0; k < h.children.length && k < 24; k++) {
        var c = h.children[k], r = c.getBoundingClientRect(), cs = getComputedStyle(c);
        lines.push('    ' + k + ' ' + c.localName + '.' + (c.className || '-') + ' @' +
          Math.round(r.left) + ',' + Math.round(r.top) + ' ' +
          Math.round(r.width) + 'x' + Math.round(r.height) +
          ' pos=' + cs.position + ' disp=' + cs.display + ' w=' + cs.width +
          ' gap=' + cs.columnGap + ' bg=' + cs.backgroundColor);
      }
    }
    var pre = document.createElement('pre');
    pre.id = 'binui-cdebug';
    pre.style.cssText = 'position:fixed;left:0;bottom:0;max-height:60%;overflow:auto;' +
      'margin:0;padding:4px;background:rgba(0,0,0,.75);color:#9fe;font:11px monospace;z-index:99999';
    pre.textContent = lines.join('\n');
    document.body.appendChild(pre);
  }

  // ---------------------------------------------------------------- 收尾
  function finish() {
    applyWindows();
    applyUnknownTags();
    shimAbsContainingBlock();
    borderNoFlow();
    dumpRects();
    dumpLayout();
    requestAnimationFrame(function () {
      requestAnimationFrame(function () {
        document.documentElement.removeAttribute('data-binui-preview-boot');
      });
    });
    state.hosts = document.querySelectorAll('[data-component]').length;
    state.done = true;
    document.documentElement.setAttribute('data-binui-preview-done',
      state.ok ? String(state.expanded) : 'error');
    // 给工具/测试一个可等的信号（binui dev 的页面、headless 抓图都用它）
    try {
      document.dispatchEvent(new CustomEvent('binui:preview', { detail: state }));
      window.dispatchEvent(new CustomEvent('binui:preview', { detail: state }));
    } catch (e) { /* 老浏览器没有 CustomEvent 构造器：不影响渲染 */ }
    if (window.console && console.info) {
      console.info('[binui-preview] 展开 ' + state.expanded + ' 个组件实例 / ' +
        state.components.length + ' 个组件文件 / 并入 ' + state.styles + ' 段组件样式' +
        ' / 补 position:' + state.absShim + ' 处' +
        (state.warnings.length ? ' / 警告 ' + state.warnings.length + ' 条' : ''));
    }
    return state;
  }

  // ---------------------------------------------------------------- 热刷新
  function attachReload() {
    if (!DEV || typeof EventSource === 'undefined') return;
    // headless 抓图/抓 DOM 时 SSE 的挂起连接会让 --virtual-time-budget 永远等下去：?noreload=1 关掉
    if (/(?:^|[?&])noreload\b/.test(location.search)) return;
    // 嵌在 binui dev 页面里的 iframe：父页面收到 reload 会重设 src，自己不用再连一条（省一半连接）
    if (window.top !== window.self && !STANDALONE) return;
    try {
      var es = new EventSource('/events');
      es.onmessage = function (ev) {
        var msg = trim(ev.data);
        if (msg === 'reload') location.reload();
      };
    } catch (e) { /* 没有 SSE 也不影响看 */ }
  }

  // ---------------------------------------------------------------- 组件独立视图
  // /__binui/c/<相对路径>?zoom=4&grid=1&host=titlebar&w=780&h=45&flex=1&text=…&<参数>=…
  var RESERVED = { zoom: 1, grid: 1, box: 1, host: 1, w: 1, h: 1, flex: 1, text: 1, bg: 1, 'class': 1 };

  function flag(q, name) {
    if (!q.has(name)) return false;
    var v = q.get(name);
    return v === '' || v === '1' || v === 'true';
  }

  // 独立视图自己的舞台样式（棋盘格看透明、居中、工具条）——只在独立视图注入
  function injectStandaloneCss() {
    var st = document.createElement('style');
    st.id = 'binui-preview-standalone';
    st.textContent = [
      'html[data-binui-preview]{height:100%}',
      'html[data-binui-preview] body{height:100%;background:#0e1116;color:#dfe6f0;' +
        'font:13px/1.5 "Segoe UI","Microsoft YaHei",sans-serif}',
      '#binui-cbar{position:fixed;left:0;right:0;top:0;height:38px;display:flex;gap:6px;' +
        'align-items:center;padding:0 10px;background:#171d26;border-bottom:1px solid #232c39;' +
        'z-index:9999;font-size:12px;color:#7d8b9e}',
      '#binui-cbar button{background:#243040;color:#dfe6f0;border:1px solid #232c39;' +
        'border-radius:5px;padding:3px 10px;cursor:pointer;font:inherit}',
      '#binui-cbar button:hover{background:#2e3d51}',
      '#binui-cbar button.on{background:#3E8BFF;border-color:#3E8BFF;color:#fff}',
      '#binui-cinfo{margin-left:auto;color:#7d8b9e}',
      '#binui-cerr{color:#ff6b6b}',
      '#binui-cstage{position:fixed;left:0;right:0;top:38px;bottom:0;display:flex;' +
        'align-items:center;justify-content:center;overflow:auto;padding:24px;' +
        'background-color:#0b0e13;' +
        'background-image:linear-gradient(45deg,#151a22 25%,transparent 25%,transparent 75%,' +
        '#151a22 75%),linear-gradient(45deg,#151a22 25%,transparent 25%,transparent 75%,#151a22 75%);' +
        'background-size:16px 16px;background-position:0 0,8px 8px}',
      '#binui-cwrap{flex:0 0 auto}',
      '#binui-cstage[data-grid="1"] #binui-chost{outline:1px solid rgba(62,139,255,.9);' +
        'outline-offset:0}',
      '#binui-cstage[data-grid="1"] #binui-chost *{outline:1px solid rgba(255,64,96,.35);' +
        'outline-offset:-1px}'
    ].join('\n');
    document.head.appendChild(st);
  }

  function standalone() {
    var path = decodeURIComponent(location.pathname.replace(/^\/__binui\/c\//, ''));
    var q = new URLSearchParams(location.search);
    var zoom = Math.min(16, Math.max(0.25, parseFloat(q.get('zoom')) || 1));
    var grid = flag(q, 'grid') || flag(q, 'box');
    document.title = 'binui dev · 组件 ' + path;

    injectStandaloneCss();

    var bar = document.createElement('div');
    bar.id = 'binui-cbar';
    bar.setAttribute('data-binui-preview-ui', '');
    var stage = document.createElement('div');
    stage.id = 'binui-cstage';
    stage.setAttribute('data-binui-preview-ui', '');
    stage.setAttribute('data-grid', grid ? '1' : '');
    var wrap = document.createElement('div');
    wrap.id = 'binui-cwrap';
    wrap.setAttribute('data-binui-preview-ui', '');
    wrap.style.zoom = String(zoom);

    // 宿主：默认 <div>，可以用 ?host=titlebar 换成组件真实的根标签（flex 排版要 ?flex=1）
    var hostTag = trim(q.get('host')) || 'div';
    if (!/^[a-z][a-z0-9-]*$/i.test(hostTag)) hostTag = 'div';
    var host = document.createElement(hostTag);
    host.id = 'binui-chost';
    host.setAttribute('data-binui-component-host', '');
    // 绝对路径走 /file/ 镜像：独立视图自己的 URL 是 /__binui/c/…，相对路径会解析错
    host.setAttribute('data-component', '/file/' + path);
    if (q.get('w')) host.style.width = toLen(q.get('w'));
    if (q.get('h')) host.style.height = toLen(q.get('h'));
    if (flag(q, 'flex')) { host.style.display = 'flex'; host.style.alignItems = 'center'; }
    if (q.get('gap')) host.style.gap = toLen(q.get('gap'));
    if (q.get('bg')) host.style.background = q.get('bg');
    if (q.get('class')) host.className = q.get('class');   // 组件皮肤类（宿主那条 class）
    if (q.get('text')) host.textContent = q.get('text');
    q.forEach(function (v, k) {
      if (RESERVED[k]) return;
      host.setAttribute('data-' + k, v);
      // 片段的 var(--x) 从祖先继承：查询参数同时当自定义属性，等于手写了一行宿主 style
      host.style.setProperty('--' + k, v);
    });

    wrap.appendChild(host);
    stage.appendChild(wrap);
    document.body.appendChild(bar);
    document.body.appendChild(stage);

    function setUrl(key, value) {
      var u = new URL(location.href);
      if (value == null) u.searchParams.delete(key); else u.searchParams.set(key, String(value));
      history.replaceState(null, '', u.toString());
    }

    function applyZoom(z) {
      zoom = Math.min(16, Math.max(0.25, z));
      wrap.style.zoom = String(zoom);
      setUrl('zoom', zoom);
      refresh();
    }

    function toggleGrid() {
      grid = !grid;
      stage.setAttribute('data-grid', grid ? '1' : '');
      setUrl('grid', grid ? '1' : null);
      refresh();
    }

    function refresh() {
      var r = host.getBoundingClientRect();
      var w = r.width / zoom, h = r.height / zoom;   // 还原成 1x 的自然尺寸
      bar.textContent = '';
      [1, 2, 4, 8].forEach(function (z) {
        var b = document.createElement('button');
        b.textContent = z + 'x';
        b.className = z === zoom ? 'on' : '';
        b.onclick = function () { applyZoom(z); };
        bar.appendChild(b);
      });
      var bz = document.createElement('button');
      bz.textContent = '盒边界';
      bz.className = grid ? 'on' : '';
      bz.onclick = toggleGrid;
      bar.appendChild(bz);
      var info = document.createElement('span');
      info.id = 'binui-cinfo';
      info.textContent = Math.round(zoom * 100) + '% · ' + Math.round(w) + '×' + Math.round(h) +
        ' · ' + path + ' · 展开 ' + state.expanded +
        (state.styles ? ' / 样式 ' + state.styles : '');
      bar.appendChild(info);
      if (state.errors.length) {
        var er = document.createElement('span');
        er.id = 'binui-cerr';
        er.textContent = ' ' + state.errors.join(' | ');
        bar.appendChild(er);
      }
    }

    document.addEventListener('keydown', function (ev) {
      if (ev.target && /^(INPUT|TEXTAREA)$/.test(ev.target.tagName)) return;
      if (ev.key === '+' || ev.key === '=') { applyZoom(zoom * 2); ev.preventDefault(); }
      else if (ev.key === '-') { applyZoom(zoom / 2); ev.preventDefault(); }
      else if (ev.key === '0') { applyZoom(1); ev.preventDefault(); }
      else if (ev.key === 'g' || ev.key === 'G') { toggleGrid(); ev.preventDefault(); }
    });

    return refresh;
  }

  // ---------------------------------------------------------------- 入口
  window.__binuiPreview = {
    state: state,
    expandAll: expandAll,
    // 手动挂一个组件：binuiPreview.mount(el, {src: 'x.bum.html', params: {...}, text: '默认文本'})
    mount: function (el, opts) {
      opts = opts || {};
      if (opts.src) el.setAttribute('data-component', opts.src);
      if (opts.text != null && !directText(el)) el.textContent = opts.text;
      var ps = opts.params || {};
      Object.keys(ps).forEach(function (k) { el.setAttribute('data-' + k, ps[k]); });
      return expand(el, 0, []);
    }
  };

  function boot() {
    injectBase();
    attachReload();
    var standaloneMode = STANDALONE || /^\/__binui\/c\//.test(location.pathname);
    if (standaloneMode) {
      var refresh = standalone();
      expandAll().then(refresh).catch(function (e) { fail('组件独立视图：' + e.message); });
    } else {
      expandAll().catch(function (e) { fail('展开组件：' + e.message); });
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
