// Issue #229: ESLint for the inline <script> of the web UI pages (idf/components/gw_web/www),
// and (#219) for the install tool of the documentation site (docs/flash/, without vendor/).
// Only the rules that find errors: the rules of eslint:recommended with the type "problem"
// (no-undef, no-unused-vars, no-dupe-keys, ...), plus no-redeclare and no-global-assign.
// No style rules. The pages are embedded in the firmware, so the lint runs in CI only, and a
// finding is corrected here (an option of the rule) unless it is a real defect of the page:
//   no-unused-vars           an unused catch parameter (catch(e){}) is not an error
//   no-irregular-whitespace  a template literal can hold U+202F (the digit groups of a number)
//   no-useless-assignment    off: it reports a defensive initial value (let j={}) that a try
//                            always replaces; such a dead store is not an error
// Run it from the root of the repository (see CLAUDE.md, section Commands): with --config, the
// paths in "files" are relative to the current directory.
import js from "@eslint/js";
import { builtinRules } from "eslint/use-at-your-own-risk";
import html from "eslint-plugin-html";
import globals from "globals";

const problems = Object.fromEntries(
    Object.entries(js.configs.recommended.rules)
        .filter(([name]) => builtinRules.get(name).meta.type === "problem"));

const rules = {
    ...problems,
    "no-redeclare": "error",
    "no-global-assign": "error",
    "no-unused-vars": ["error", { caughtErrors: "none" }],
    "no-irregular-whitespace": ["error", { skipTemplates: true }],
    "no-useless-assignment": "off",
};

export default [
    {
        ignores: ["docs/flash/vendor/**"],
    },
    {
        files: ["idf/components/gw_web/www/*.html"],
        plugins: { html },
        languageOptions: {
            ecmaVersion: 2022,
            sourceType: "script",
            globals: { ...globals.browser },
        },
        linterOptions: { reportUnusedDisableDirectives: "error" },
        rules,
    },
    {
        // the modules of the tool (boot.js loads app.js with import())
        files: ["docs/flash/*.js"],
        ignores: ["docs/flash/boot.js"],
        languageOptions: { ecmaVersion: 2022, sourceType: "module", globals: { ...globals.browser } },
        linterOptions: { reportUnusedDisableDirectives: "error" },
        rules,
    },
    {
        // a classic script of each page of the site (extra_javascript of zensical.toml)
        files: ["docs/flash/boot.js"],
        languageOptions: { ecmaVersion: 2022, sourceType: "script", globals: { ...globals.browser } },
        linterOptions: { reportUnusedDisableDirectives: "error" },
        rules,
    },
];
