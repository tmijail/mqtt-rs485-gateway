# The E2E image (idf/tools/e2e/Dockerfile) plus Playwright with headless Chromium, for the
# screenshots of the web UI in docs/images (idf/tools/docs_shots.py). Build gw-e2e first, then
# this image from this directory:
#
#   docker build -t gw-e2e idf/tools/e2e
#   docker build -t gw-docs idf/tools/docs
#
# See docs/how-to/make-the-docs-screenshots.md. BASE selects another tag of the E2E image.
ARG BASE=gw-e2e
FROM ${BASE}

# Chromium in a fixed place, so that the browser of the image does not depend on $HOME
ENV PLAYWRIGHT_BROWSERS_PATH=/opt/ms-playwright
COPY requirements.txt /opt/docs-requirements.txt
# The font of the images (Inter) and its settings: the web UI asks for system-ui, which has no
# fixed font in a container. With them, each host draws the same pixels (56-gw-docs-fonts.conf)
COPY 56-gw-docs-fonts.conf /etc/fonts/conf.d/56-gw-docs-fonts.conf
RUN /opt/e2e/bin/pip install -q -r /opt/docs-requirements.txt \
    && /opt/e2e/bin/python -m playwright install --with-deps chromium >/dev/null \
    && apt-get update -qq \
    && apt-get install -y -qq --no-install-recommends fonts-inter >/dev/null \
    && fc-cache -f \
    && rm -rf /var/lib/apt/lists/*
