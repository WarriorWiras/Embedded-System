# 🔬 SPI Flash Performance Evaluation & Forensic Analysis

**Embedded Systems Project – Raspberry Pi Pico W**

![Platform](https://img.shields.io/badge/Platform-Raspberry%20Pi%20Pico%20W-blue)
![Language](https://img.shields.io/badge/Language-C-brightgreen)
![Storage](https://img.shields.io/badge/Storage-SPI%20Flash%20%26%20microSD-orange)
![Network](https://img.shields.io/badge/Network-Wi--Fi%20Dashboard-yellow)
![Purpose](https://img.shields.io/badge/Purpose-Educational%20%26%20Forensic-lightgrey)

---

## 📘 About This Repository

This repository contains an **embedded systems project** focused on **benchmarking, analysing, and forensically differentiating SPI flash memory chips** using a **Raspberry Pi Pico W**.

Although many SPI flash chips share identical storage capacities, their **internal architectures and operational behaviour** (read, program, erase timings) can differ in subtle but measurable ways. This project demonstrates how **high-resolution timing, structured logging, and repeatable workloads** can be used to uncover those differences.

The system is designed as a **self-contained embedded platform**, capable of:

* Interfacing directly with SPI flash chips
* Logging benchmark data to microSD
* Hosting a lightweight **Wi-Fi web dashboard**
* Supporting repeatable and forensic-grade analysis

> 🎯 **Primary Goal:**
> Build a reliable, repeatable, and defensible SPI flash benchmarking and analysis tool under real embedded constraints.

---

## 🧠 High-Level System Overview

At a high level, the system consists of:

* **Raspberry Pi Pico W** as the main controller
* **SPI Flash (DUT)** as the device under test
* **microSD card** for persistent CSV logging and reports
* **Wi-Fi Access Point + HTTP dashboard** for result retrieval

The Pico W handles:

* SPI read / write / erase benchmarking
* Microsecond-level timing measurements
* CSV logging with strict schema guarantees
* Forensic analysis and report generation
* Hosting a browser-accessible dashboard without external infrastructure

All benchmarking and analysis occurs **locally on the device**, ensuring consistency and repeatability.

---

## 📂 Repository Structure

This repository is organised into **three main folders**, each with a clear and intentional purpose:

```
.
├── Driver Testing/
├── Output from Test/
├── Project/
└── README.md
```

### 1️⃣ Driver Testing

📁 `Driver Testing/`

Contains **standalone test code** used to validate individual hardware components before full system integration.

Includes tests for:

* microSD read/write functionality
* SPI communication
* Wi-Fi connectivity on Maker Pi Pico W

> Purpose: verify hardware and drivers **in isolation**.

---

### 2️⃣ Output from Test

📁 `Output from Test/`

Stores **raw output data** generated during benchmarking and testing, such as:

* CSV performance logs
* Timing datasets
* Experimental outputs

> Purpose: preserve **measurement evidence** for analysis and reporting.

---

### 3️⃣ Project

📁 `Project/`

Contains the **complete integrated source code**, including:

* SPI flash drivers
* Benchmark orchestration
* CSV logging subsystem
* Wi-Fi access point and HTTP dashboard
* Forensic analysis logic

📌 This folder has its **own `README.md`** documenting internal architecture and modules.

---

## 🛠️ Tools & Technologies

* Raspberry Pi Pico W (RP2040)
* C (Pico SDK)
* SPI protocol
* microSD (FAT32 via FatFS)
* Wi-Fi (AP mode + HTTP)
* CSV-based data logging

---

## ⚠️ Disclaimer

> 🚨 **Educational Use Only**

This repository was created as part of an **academic embedded systems project**.

* All testing is conducted on hardware owned by the project team
* No proprietary firmware is reverse-engineered
* Results are for **learning and comparison only**, not vendor certification

The authors are not responsible for misuse of this project or its results.

---

## 🤝 Contributing

Contributions are welcome for **educational improvements**.

1. Fork the repository
2. Create a feature or fix branch
3. Keep changes modular and documented
4. Submit a pull request with a clear explanation

---

## 📄 License

This project is licensed under the **MIT License**.

You are free to use, modify, and distribute this project for **educational and non-commercial purposes**, provided proper attribution is given.

See the `LICENSE` file for full details.

---

✨ *Measure carefully. Log precisely. Analyse responsibly.*
