FROM node:26-alpine AS build

WORKDIR /app

COPY package*.json ./
RUN npm ci

COPY tsconfig.json ./
COPY src ./src
RUN npm run build

FROM node:26-alpine

ENV NODE_ENV=production

WORKDIR /app

# Production deps only; sharp pulls in its prebuilt linuxmusl binaries here
COPY package*.json ./
RUN npm ci --omit=dev && npm cache clean --force

COPY --from=build /app/dist ./dist

USER node

EXPOSE 3001

CMD ["node", "dist/index.js"]
