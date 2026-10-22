#!/usr/bin/env bash
# Builds the backend and frontend images and pushes them to Docker Hub.
# Deployment: ~/Projects/osiris/kubernetes-operations/matomo-notifier
set -euo pipefail

PLATFORM="linux/amd64"
REGISTRY="osirisguitar"

cd "$(dirname "$0")"

echo "==> Building and pushing ${REGISTRY}/matomo-notifier-backend"
docker build --platform "${PLATFORM}" -t "${REGISTRY}/matomo-notifier-backend:latest" backend
docker push "${REGISTRY}/matomo-notifier-backend:latest"

echo "==> Building and pushing ${REGISTRY}/matomo-notifier-frontend"
docker build --platform "${PLATFORM}" -t "${REGISTRY}/matomo-notifier-frontend:latest" frontend
docker push "${REGISTRY}/matomo-notifier-frontend:latest"

echo "==> Done. Roll out with:"
echo "    kubectl --context mimas -n matomo-notifier rollout restart deployment"
