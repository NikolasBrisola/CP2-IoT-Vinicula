-- ============================================================================
--  CHECKPOINT IoT — Schema MySQL
--  Banco: vinicola_iot
--  Otimizado para séries temporais de monitoramento
-- ============================================================================

CREATE DATABASE IF NOT EXISTS vinicola_iot
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_unicode_ci;

USE vinicola_iot;

-- ----------------------------------------------------------------------------
-- Tabela principal de telemetria
-- Particionamento por data recomendado em produção (ver comentário abaixo)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS telemetria (
  id             BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  dispositivo    VARCHAR(50)    NOT NULL DEFAULT 'esp32_vinicola_cp1',
  temperatura    DECIMAL(5,2)   NOT NULL COMMENT '°C — faixa ideal 12-18',
  umidade        DECIMAL(5,2)   NOT NULL COMMENT '% — faixa ideal 60-70',
  luminosidade   INT UNSIGNED   NOT NULL COMMENT 'ADC 0-4095, quanto menor melhor',
  alerta         VARCHAR(100)   NOT NULL DEFAULT 'ok' COMMENT 'Classificação de alerta',
  msg_id         BIGINT UNSIGNED         COMMENT 'Sequencial do ESP32',
  uptime_s       BIGINT UNSIGNED         COMMENT 'Segundos desde boot do ESP32',
  created_at     TIMESTAMP      NOT NULL DEFAULT CURRENT_TIMESTAMP,

  -- Índice composto para queries temporais por dispositivo
  -- Ex: SELECT * FROM telemetria WHERE dispositivo = 'x' AND created_at > NOW() - INTERVAL 1 HOUR
  INDEX idx_dispositivo_tempo (dispositivo, created_at DESC),

  -- Índice para alertas (dashboard de incidentes)
  INDEX idx_alerta (alerta, created_at DESC)

) ENGINE=InnoDB;

-- ----------------------------------------------------------------------------
-- Tabela para dados do clima externo (OpenWeather)
-- Correlaciona ambiente externo vs. interno da vinícola
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS clima_externo (
  id             BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  cidade         VARCHAR(100)   NOT NULL DEFAULT 'Sao Paulo',
  temp_externa   DECIMAL(5,2)   NOT NULL COMMENT '°C',
  umidade_ext    DECIMAL(5,2)   NOT NULL COMMENT '%',
  descricao      VARCHAR(200)            COMMENT 'Ex: céu limpo, chuva leve',
  created_at     TIMESTAMP      NOT NULL DEFAULT CURRENT_TIMESTAMP,

  INDEX idx_cidade_tempo (cidade, created_at DESC)

) ENGINE=InnoDB;

-- ----------------------------------------------------------------------------
-- View útil: último registro de cada dispositivo (status rápido no dashboard)
-- ----------------------------------------------------------------------------
CREATE OR REPLACE VIEW v_ultimo_status AS
SELECT t.*
FROM telemetria t
INNER JOIN (
  SELECT dispositivo, MAX(id) AS max_id
  FROM telemetria
  GROUP BY dispositivo
) latest ON t.id = latest.max_id;

-- ----------------------------------------------------------------------------
-- Procedure para limpar registros antigos (retenção de dados)
-- Chamada via cron ou Event Scheduler do MySQL
-- Padrão: mantém 90 dias
-- ----------------------------------------------------------------------------
DELIMITER //
CREATE PROCEDURE IF NOT EXISTS limpar_telemetria_antiga(IN dias_retencao INT)
BEGIN
  DELETE FROM telemetria
  WHERE created_at < NOW() - INTERVAL dias_retencao DAY;

  DELETE FROM clima_externo
  WHERE created_at < NOW() - INTERVAL dias_retencao DAY;
END //
DELIMITER ;

-- Exemplo de uso: CALL limpar_telemetria_antiga(90);

-- ----------------------------------------------------------------------------
-- Event Scheduler (opcional): limpeza automática semanal
-- Requer: SET GLOBAL event_scheduler = ON;
-- ----------------------------------------------------------------------------
-- CREATE EVENT IF NOT EXISTS evt_limpeza_semanal
-- ON SCHEDULE EVERY 1 WEEK
-- STARTS CURRENT_TIMESTAMP
-- DO CALL limpar_telemetria_antiga(90);
