# Компонент: infra

Инфраструктурный слой: Docker-образы, docker-compose для dev, Kubernetes манифесты, скрипты для Kafka.

## Файлы

| Файл | Назначение |
|---|---|
| [docker/Dockerfile.service](../../docker/Dockerfile.service) | Универсальный Dockerfile для C++ сервисов |
| [infra/docker-compose.dev.yml](../../infra/docker-compose.dev.yml) | Dev-стенд: все сервисы + Redpanda |
| [infra/env/](../../infra/env/) | env-файлы для compose |
| [infra/k8s/gateway.yaml](../../infra/k8s/gateway.yaml) | k8s manifests для gateway |
| [infra/k8s/order_flow.yaml](../../infra/k8s/order_flow.yaml) | k8s manifests для order_flow |
| [infra/kafka/create_topics.sh](../../infra/kafka/create_topics.sh) | Создание Kafka топиков через rpk |

## Kafka топики

См. [../03-architecture/communication.md](../03-architecture/communication.md) и [../06-api/messaging/topics.md](../06-api/messaging/topics.md). Скрипт создания:

```bash
./infra/kafka/create_topics.sh
```

| Топик | Retention |
|---|---|
| `marketdata.raw` | 1 час |
| `orders.normalized` | 7 дней |
| `batch.outputs` | 7 дней |
| `execution.intents` | 1 день |
| `execution.reports` | 7 дней |
| `risk.alerts` | 30 дней |

## Запуск локально

См. [Development.md](../../Development.md) и [README.md](../../README.md).

## Не реализовано

- k8s manifests только для gateway и order_flow; остальные сервисы — TODO.
- Нет Helm-чартов.
- Нет Terraform / IaC.
- Нет mTLS и сертификатов.
- Нет network policies.
