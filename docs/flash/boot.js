// Issue #219: loads the install tool (app.js) only on the page that has the element #gw-flash.
// zensical.toml loads this small script on each page (extra_javascript). With the instant
// navigation of the theme, a page change does not load the scripts again: the observable
// document$ of the theme tells each new page.
(function () {
    "use strict";
    var appUrl = new URL("app.js", document.currentScript.src).href;
    var current = null;   // {root, tool}

    function onPage() {
        if (current && !document.body.contains(current.root)) {
            current.tool.then(function (t) { return t && t.unmount(); });
            current = null;
        }
        var root = document.getElementById("gw-flash");
        if (!root || (current && current.root === root)) {
            return;
        }
        root.classList.add("gwf");
        current = {
            root: root,
            tool: import(appUrl).then(function (m) { return m.mount(root); }).catch(function (e) {
                root.innerHTML = "<p><b>The tool did not start.</b> Load the page again. Detail: " +
                    String(e && e.message).replace(/[<&]/g, "") + "</p>";
                return null;
            }),
        };
    }

    if (window.document$ && typeof window.document$.subscribe === "function") {
        window.document$.subscribe(onPage);
    } else if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", onPage);
    } else {
        onPage();
    }
})();
