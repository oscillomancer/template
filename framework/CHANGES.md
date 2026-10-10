# Framework changes

The framework is versioned on its own (`framework/VERSION`) so a product can
tell which one it carries. Each entry says what a product must do, if
anything, after `framework/tools/update_framework.cmake` brings it in.

## 1.0.0

The rebuild. Products move onto it by porting their DSP into `product/`
(see `framework/AGENTS.md`); there is no automatic migration from the
earlier per-product templates. A product with sessions in the wild must
implement `Processor::loadLegacyState` for its old state format, and keep its
plugin ID, AU subtype and parameter IDs exactly as they shipped.
