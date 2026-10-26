# Backend image for the remote / memory-capped benchmark (benchmark/remote/README.md).
#
# The same service as deploy/copilot-llm/Dockerfile.backend, plus iproute2 so the
# run script can shape the container's link with `tc netem` (latency and
# bandwidth), which emulates a service on another host. Build from the
# repository root:
#
#   docker build -f benchmark/remote/Dockerfile -t visivo-bench-backend .
FROM python:3.11-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libgl1 libglib2.0-0 libgomp1 curl iproute2 build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app/backend
COPY backend/requirements.txt ./requirements.txt
RUN pip install --no-cache-dir -r requirements.txt

COPY backend/ /app/backend/

ENV PYTHONUNBUFFERED=1
EXPOSE 8000
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000", "--no-access-log"]
