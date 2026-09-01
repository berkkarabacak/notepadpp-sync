# syntax=docker/dockerfile:1

# ---- build stage ----
FROM golang:1.22-alpine AS build
WORKDIR /src
RUN apk add --no-cache ca-certificates
COPY . .
# go.sum may be absent in a fresh checkout; resolve dependencies now
# (must happen AFTER sources are copied or tidy would drop all requirements).
RUN go mod tidy && go mod download
RUN CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o /out/server ./cmd/server

# ---- runtime stage ----
FROM alpine:3.20
RUN apk add --no-cache ca-certificates && adduser -D -u 10001 npsync \
    && mkdir -p /data/blobs && chown -R npsync:npsync /data
USER npsync
WORKDIR /app
COPY --from=build /out/server /app/server
VOLUME ["/data/blobs"]
EXPOSE 8080
ENV NPSYNC_LISTEN_ADDR=":8080" \
    NPSYNC_BLOB_FS_DIR="/data/blobs"
HEALTHCHECK --interval=30s --timeout=3s --retries=3 \
  CMD wget -qO- http://127.0.0.1:8080/health || exit 1
ENTRYPOINT ["/app/server"]
