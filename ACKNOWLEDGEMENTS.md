# Acknowledgements / 致謝

## OpenAI ChatGPT contribution

This project was developed with substantial assistance from **OpenAI ChatGPT (GPT-5.6 Sol)**.

ChatGPT contributed materially to:

- analysis of the Sony Alpha BLE location-information protocol and public reverse-engineering references;
- debugging the ESP32-C6 / ESP-IDF Bluedroid pairing and bonding flow;
- iterative firmware design and diagnosis of the successful v6 proof-of-concept;
- interpretation of BLE/GATT logs, security events, MTU negotiation, and Sony DD00/DD11/DD21 behavior;
- validation planning for RAW/EXIF GPS output;
- documentation, repository structure, bilingual English / Traditional Chinese writing, and release preparation;
- GNSS, power, flash-logging, low-power, and future PCB architecture planning.

The repository owner performed the physical hardware work, camera operation, on-device testing, pairing confirmation, RAW capture, real-world verification, component selection decisions, and final approval of published changes.

The intent of this acknowledgement is to make the project's AI-assisted development process explicit rather than presenting the work as entirely human-authored.

---

## 🇹🇼 OpenAI ChatGPT 的貢獻

本專案在開發過程中大量使用 **OpenAI ChatGPT（GPT-5.6 Sol）** 協作。

ChatGPT 對下列工作有實質貢獻：

- Sony Alpha BLE 位置資訊協定與公開 reverse-engineering 資料分析；
- ESP32-C6 / ESP-IDF Bluedroid 配對、加密與 Bond persistence 除錯；
- 多版韌體迭代，以及最終成功 v6 PoC 的診斷與整理；
- BLE / GATT log、SMP security event、MTU negotiation、DD00 / DD11 / DD21 行為判讀；
- RAW / EXIF GPS 端到端驗證方法設計；
- GitHub 文件、專案架構、英文與繁體中文說明及發布整理；
- 後續 GNSS、電源、Flash 軌跡記錄、低功耗與正式 PCB 架構規劃。

實體硬體組裝、Sony 相機操作、實機配對確認、RAW 拍攝、真實環境驗證、零件選擇與最終發布決策，均由 repository owner 完成與確認。

特別列出本頁，是希望清楚揭露這是一個 **human + AI collaborative development** 專案，而不是把 AI 的實質協作隱藏起來。
