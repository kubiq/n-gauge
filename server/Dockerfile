# The gauge's log upload service - see README.md.
FROM python:3.13-alpine
WORKDIR /app
COPY upload_server.py .
# Not root: it only writes UPLOAD_DIR.
RUN adduser -D -u 1000 gauge && mkdir /data && chown gauge /data
USER gauge
ENV UPLOAD_DIR=/data UPLOAD_PORT=8650 PYTHONUNBUFFERED=1
EXPOSE 8650
CMD ["python3", "upload_server.py"]
