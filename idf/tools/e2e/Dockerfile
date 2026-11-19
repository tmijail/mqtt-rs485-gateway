# The E2E environment of the CI in one image: the pinned ESP-IDF v6.1 image of
# .github/workflows/ci.yml plus what its e2e jobs install (mosquitto, libslirp0, QEMU for Xtensa
# and the harness' Python packages). Build it from this directory:
#
#   docker build -t gw-e2e idf/tools/e2e
#
# See docs/how-to/run-e2e-in-docker.md. Issue #253: .github/workflows/e2e-image.yml publishes it
# as ghcr.io/tmijail/gw-e2e, and the e2e jobs of ci.yml run in it (pinned by digest).
FROM espressif/idf:v6.1@sha256:f81b6e077e6af93c312181cd7d4e7936cfbafc5c7f77cbacba8fdd34994f218a
LABEL org.opencontainers.image.source="https://github.com/tmijail/mqtt-rs485-gateway" \
      org.opencontainers.image.description="The E2E environment of the RS485 to Sparkplug B gateway (QEMU, mosquitto, the harness)"

RUN apt-get -o Acquire::Retries=3 update -qq \
    && apt-get -o Acquire::Retries=3 install -y -qq --no-install-recommends mosquitto libslirp0 >/dev/null \
    && rm -rf /var/lib/apt/lists/*
RUN python3 "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa

COPY requirements.txt /opt/e2e-requirements.txt
RUN python3 -m venv /opt/e2e && /opt/e2e/bin/pip install -q -r /opt/e2e-requirements.txt

# mosquitto is in /usr/sbin (the CI adds it to PATH in the same way)
ENV PATH="$PATH:/usr/sbin" \
    E2E_PYTHON=/opt/e2e/bin/python
