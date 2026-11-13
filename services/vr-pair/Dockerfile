# VisIVO VR pairing service (visivo-pair/1). Small and stateless: codes live
# in memory only. Configuration through VR_PAIR_* variables (README.md).
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    VR_PAIR_BIND=0.0.0.0 \
    VR_PAIR_PORT=8765

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY vr_pair_service.py .

RUN useradd --system --uid 10001 --no-create-home vrpair
USER vrpair

EXPOSE 8765
HEALTHCHECK --interval=30s --timeout=3s \
  CMD python -c "import os,urllib.request; urllib.request.urlopen('http://127.0.0.1:%s/healthz' % os.environ.get('VR_PAIR_PORT','8765'), timeout=2)" || exit 1

CMD ["python", "vr_pair_service.py"]
