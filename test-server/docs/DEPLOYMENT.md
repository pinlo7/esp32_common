# 部署文档

## 一、环境要求

### 云服务器

| 项目 | 最低要求 | 推荐 |
|------|---------|------|
| CPU | 1 核 | 2 核 |
| 内存 | 1 GB | 2 GB |
| 磁盘 | 20 GB | 50 GB |
| 系统 | Ubuntu 22.04 | Ubuntu 22.04 LTS |

### 软件依赖

| 软件 | 版本 |
|------|------|
| Docker | 24+ |
| Docker Compose | 2.20+ |

---

## 二、项目结构

```
my-devices-project/
├── backend/
│   ├── src/
│   ├── prisma/
│   ├── Dockerfile
│   └── package.json
├── frontend/
│   ├── src/
│   ├── Dockerfile
│   └── package.json
├── docker-compose.yml
├── .env
└── README.md
```

---

## 三、环境变量

### .env

```bash
# 数据库
POSTGRES_USER=devices
POSTGRES_PASSWORD=your_strong_password
POSTGRES_DB=devices_db

# 后端
DATABASE_URL=postgresql://devices:your_strong_password@postgres:5432/devices_db?schema=public
JWT_SECRET=your_jwt_secret_key_at_least_32_chars
JWT_EXPIRES_IN=7d

# EMQX
EMQX_USERNAME=admin
EMQX_PASSWORD=your_emqx_password

# MQTT（后端连接用）
MQTT_URL=mqtt://emqx:1883
MQTT_USERNAME=backend
MQTT_PASSWORD=backend_password

# 固件存储
FIRMWARE_UPLOAD_DIR=/data/firmware
FIRMWARE_BASE_URL=https://your-domain.com/firmware
```

---

## 四、Docker Compose

```yaml
version: '3.8'

services:
  postgres:
    image: postgres:16-alpine
    restart: unless-stopped
    environment:
      POSTGRES_USER: ${POSTGRES_USER}
      POSTGRES_PASSWORD: ${POSTGRES_PASSWORD}
      POSTGRES_DB: ${POSTGRES_DB}
    volumes:
      - postgres_data:/var/lib/postgresql/data
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U ${POSTGRES_USER}"]
      interval: 10s
      timeout: 5s
      retries: 5

  emqx:
    image: emqx/emqx:5
    restart: unless-stopped
    environment:
      EMQX_NAME: devices_emqx
      EMQX_DASHBOARD__DEFAULT_USERNAME: ${EMQX_USERNAME:-admin}
      EMQX_DASHBOARD__DEFAULT_PASSWORD: ${EMQX_PASSWORD:-public}
    ports:
      - "1883:1883"
      - "8083:8083"
      - "18083:18083"
    volumes:
      - emqx_data:/opt/emqx/data
      - emqx_log:/opt/emqx/log

  backend:
    build: ./backend
    restart: unless-stopped
    environment:
      DATABASE_URL: ${DATABASE_URL}
      JWT_SECRET: ${JWT_SECRET}
      JWT_EXPIRES_IN: ${JWT_EXPIRES_IN:-7d}
      MQTT_URL: ${MQTT_URL}
      MQTT_USERNAME: ${MQTT_USERNAME}
      MQTT_PASSWORD: ${MQTT_PASSWORD}
      FIRMWARE_UPLOAD_DIR: /data/firmware
      FIRMWARE_BASE_URL: ${FIRMWARE_BASE_URL}
    volumes:
      - firmware_data:/data/firmware
    depends_on:
      postgres:
        condition: service_healthy
      emqx:
        condition: service_started
    ports:
      - "3000:3000"

  frontend:
    build: ./frontend
    restart: unless-stopped
    ports:
      - "80:80"
    depends_on:
      - backend

  nginx:
    image: nginx:alpine
    restart: unless-stopped
    ports:
      - "443:443"
    volumes:
      - ./nginx/nginx.conf:/etc/nginx/nginx.conf
      - ./nginx/ssl:/etc/nginx/ssl
      - firmware_data:/usr/share/nginx/html/firmware
    depends_on:
      - frontend
      - backend

volumes:
  postgres_data:
  emqx_data:
  emqx_log:
  firmware_data:
```

---

## 五、后端 Dockerfile

```dockerfile
# backend/Dockerfile
FROM node:22-alpine AS builder

WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY . .
RUN npx prisma generate
RUN npm run build

FROM node:22-alpine

WORKDIR /app
COPY --from=builder /app/node_modules ./node_modules
COPY --from=builder /app/dist ./dist
COPY --from=builder /app/prisma ./prisma
COPY --from=builder /app/package.json ./

EXPOSE 3000

CMD ["sh", "-c", "npx prisma migrate deploy && node dist/index.js"]
```

---

## 六、前端 Dockerfile

```dockerfile
# frontend/Dockerfile
FROM node:22-alpine AS builder

WORKDIR /app
COPY package*.json ./
RUN npm ci
COPY . .
RUN npm run build

FROM nginx:alpine

COPY --from=builder /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80
```

---

## 七、Nginx 配置

### 反向代理

```nginx
# nginx/nginx.conf
events {
    worker_connections 1024;
}

http {
    include       /etc/nginx/mime.types;
    default_type  application/octet-stream;

    upstream backend {
        server backend:3000;
    }

    upstream frontend {
        server frontend:80;
    }

    server {
        listen 80;
        server_name your-domain.com;
        return 301 https://$server_name$request_uri;
    }

    server {
        listen 443 ssl http2;
        server_name your-domain.com;

        ssl_certificate /etc/nginx/ssl/fullchain.pem;
        ssl_certificate_key /etc/nginx/ssl/privkey.pem;

        # 前端
        location / {
            proxy_pass http://frontend;
        }

        # API
        location /api/ {
            proxy_pass http://backend;
            proxy_set_header X-Real-IP $remote_addr;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_set_header X-Forwarded-Proto $scheme;
        }

        # 固件下载
        location /firmware/ {
            alias /usr/share/nginx/html/firmware/;
            add_header Content-Disposition "attachment";
        }
    }
}
```

---

## 八、部署步骤

### 1. 服务器准备

```bash
# 更新系统
sudo apt update && sudo apt upgrade -y

# 安装 Docker
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER

# 重新登录
logout
```

### 2. 克隆项目

```bash
cd /opt
git clone <repository-url> my-devices-project
cd my-devices-project
```

### 3. 配置环境变量

```bash
cp .env.example .env
nano .env
```

修改数据库密码、JWT 密钥等。

### 4. 启动服务

```bash
# 构建并启动
docker compose up -d --build

# 查看状态
docker compose ps
```

### 5. 初始化数据库

```bash
# 运行迁移
docker compose exec backend npx prisma migrate deploy

# 创建种子数据
docker compose exec backend npx tsx prisma/seed.ts
```

### 6. 配置 HTTPS

```bash
# 安装 Certbot
sudo apt install certbot python3-certbot-nginx -y

# 获取证书
sudo certbot --nginx -d your-domain.com

# 自动续期
sudo certbot renew --dry-run
```

### 7. 配置防火墙

```bash
sudo ufw allow 22/tcp    # SSH
sudo ufw allow 80/tcp    # HTTP
sudo ufw allow 443/tcp   # HTTPS
sudo ufw allow 1883/tcp  # MQTT
sudo ufw allow 8083/tcp  # MQTT WebSocket
sudo ufw allow 18083/tcp # EMQX Dashboard
sudo ufw enable
```

---

## 九、维护命令

### 查看日志

```bash
# 所有服务
docker compose logs -f

# 特定服务
docker compose logs -f backend
docker compose logs -f emqx
```

### 重启服务

```bash
docker compose restart backend
```

### 更新部署

```bash
git pull
docker compose down
docker compose up -d --build
docker compose exec backend npx prisma migrate deploy
```

### 备份数据库

```bash
docker compose exec postgres pg_dump -U devices devices_db > backup_$(date +%Y%m%d).sql
```

### 恢复数据库

```bash
docker compose exec -T postgres psql -U devices devices_db < backup.sql
```

---

## 十、监控

### 服务状态

```bash
docker compose ps
```

### 资源使用

```bash
docker stats
```

### EMQX Dashboard

访问 `https://your-domain.com:18083`

---

## 十一、故障排查

### 数据库连接失败

```bash
docker compose logs postgres
docker compose exec postgres psql -U devices devices_db
```

### MQTT 连接失败

```bash
docker compose logs emqx
# 检查端口
netstat -tlnp | grep 1883
```

### 后端启动失败

```bash
docker compose logs backend
# 检查环境变量
docker compose exec backend env
```