# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, AU codes, window class, state tag,
# installed file names and test configuration are all generated from it.
#
# PRODUCT_ID, PRODUCT_AU_SUBTYPE and PRODUCT_STATE_TAG identify saved sessions
# and must never change once the product has shipped.

set(PRODUCT_NAME        "Starter")                  # display name and bundle file name
set(PRODUCT_ID          "com.suppleaudio.starter")  # reverse-DNS CLAP and bundle identifier
set(PRODUCT_VERSION     "0.1.0")                    # semantic version
set(PRODUCT_DESCRIPTION "A transparent starting point for a new audio effect.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

# effect: one main input and output bus of equal width.
# instrument: note input, one main output, no audio input.
set(PRODUCT_KIND        "effect")

# Main bus widths offered to the host, the first being the default. Effects
# run in == out; an instrument uses these as its output width.
set(PRODUCT_CHANNELS    2 1)

# CLAP features after the main category (audio-effect or instrument, which is
# implied by PRODUCT_KIND). clap-wrapper maps them onto VST3 subcategories.
set(PRODUCT_FEATURES    "utility" "filter" "stereo" "mono")

# Four ASCII characters each. Supl is Supple Audio's manufacturer code, shared
# by every product; the subtype alone tells products apart.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "StFx")
# The first four bytes of every saved state. Unique per product.
set(PRODUCT_STATE_TAG       "StFx")

# Product sources beyond the headers in product/. Paths relative to the
# repository root.
set(PRODUCT_DSP_SOURCES     "")
set(PRODUCT_EDITOR_SOURCES  product/Editor.cpp)
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

# host-smoke: a parameter whose automation must never change latency, and,
# if the product has one, a parameter that does (the test then checks the
# restart protocol around it).
set(PRODUCT_SMOKE_PLAIN_PARAM   "Input Gain")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
