FROM node:24-alpine

WORKDIR /app
# No dependencies to install: server.js uses only Node's own modules.
COPY package.json server.js ./

# The logs live in a volume, owned by the image's unprivileged `node` user.
RUN mkdir -p /data && chown node:node /data
USER node
VOLUME /data

ENV PORT=8080 DATA_DIR=/data
EXPOSE 8080
HEALTHCHECK --interval=60s --timeout=5s \
  CMD wget -qO- http://127.0.0.1:8080/api/health || exit 1

CMD ["node", "server.js"]
