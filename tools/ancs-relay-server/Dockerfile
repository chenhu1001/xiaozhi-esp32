FROM golang:1.25-bookworm AS builder

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY *.go ./
RUN CGO_ENABLED=0 GOOS=linux go test ./... && \
    CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /out/ancs-relay-server . && \
    mkdir /out/data

FROM scratch

COPY --from=builder /out/ancs-relay-server /ancs-relay-server
COPY --from=builder --chown=10001:10001 /out/data /data
USER 10001:10001
VOLUME ["/data"]
EXPOSE 8080
ENV ANCS_LISTEN_ADDR=:8080
ENV ANCS_DB_FILE=/data/ancs.db
ENTRYPOINT ["/ancs-relay-server"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["/ancs-relay-server", "healthcheck"]
