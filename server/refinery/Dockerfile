# Bedrock's refinery observations: Python's standard library and SQLite,
# nothing installed besides
FROM python:3.13-slim

RUN useradd --system --uid 10001 --no-create-home bedrock \
    && mkdir -p /app /data \
    && chown bedrock /data

WORKDIR /app
COPY app.py stations.txt tables.json ./

USER bedrock
ENV BEDROCK_API_DB=/data/refinery.db \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1
EXPOSE 8080
VOLUME ["/data"]

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD ["python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/bedrock/api/v1/health', timeout=4)"]

CMD ["python", "app.py"]
