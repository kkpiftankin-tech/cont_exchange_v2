FROM node:20-alpine

WORKDIR /app

COPY package*.json /app/
RUN npm ci
COPY public /app/public
COPY src /app/src

ENV HOST=0.0.0.0
ENV PORT=3000

EXPOSE 3000

CMD ["npm", "start"]
