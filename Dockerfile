FROM python:3.10-slim

WORKDIR /app

# Install dependencies first (layer caching)
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy source and install package
COPY pyproject.toml .
COPY src/ src/
RUN pip install --no-cache-dir -e .

EXPOSE 10000

CMD sh -c "gunicorn weight_room.api:app --worker-class uvicorn.workers.UvicornWorker --bind 0.0.0.0:10000 --timeout 120 --workers ${WEIGHT_ROOM_GUNICORN_WORKERS:-2}"
