FROM node:20-alpine

WORKDIR /app

COPY package*.json ./
RUN npm install

COPY proto ./proto

COPY server.js .
COPY data ./data

EXPOSE 8090

ENV PORT=8090

CMD ["node", "server.js"]