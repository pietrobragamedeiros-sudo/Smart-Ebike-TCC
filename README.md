# Kit Inteligente Smart E-Bike

Trabalho de Conclusão de Curso (TCC) apresentado ao Curso Técnico em Eletrônica da **ETEC Albert Einstein** (São Paulo, 2026).

**Autores:**
* Pietro Braga Medeiros Passos
* Pedro Taveira Araújo

---

## Sobre o Projeto
Sistema embarcado modular de baixo custo desenvolvido para converter bicicletas convencionais em bicicletas elétricas inteligentes em menos de 10 minutos, integrando assistência inteligente ao pedal (PAS), modos de pilotagem (`ECO` e `SPORT`), telemetria em tempo real e alerta de emergência (`S.O.S.`) com localização via GPS.

## Estrutura dos Códigos
* **`Esp_Solo.ino`**: Firmware do microcontrolador ESP32 principal, responsável pela leitura dos sensores magnéticos, controle de aceleração via conversor DAC, comunicação sem fio (`ESP-NOW`), recepção de coordenadas GPS e servidor web de telemetria.
* **`Tela_Tcc.ino`**: Firmware da interface gráfica sensível ao toque (`ESP32-2432S028` / Tela CYD), desenvolvido com a biblioteca LVGL para exibição de velocidade (km/h), seleção de modos e acionamento do botão de emergência S.O.S.
