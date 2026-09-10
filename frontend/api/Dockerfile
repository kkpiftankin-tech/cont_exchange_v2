FROM node:20-alpine

WORKDIR /app

COPY package*.json ./
RUN npm install

# proto/ — ГЕНЕРИРУЕМАЯ копия contracts/proto/ (T-CEA-003). Перед сборкой запустить
# `sh frontend/api/sync-proto.sh` (build context = frontend/api/, поэтому контракты
# синхронизируются на хосте, не в Dockerfile). Не редактировать proto/ руками — дрейф.
COPY proto ./proto

COPY server.js .
COPY data ./data

EXPOSE 8090

ENV PORT=8090

CMD ["node", "server.js"]