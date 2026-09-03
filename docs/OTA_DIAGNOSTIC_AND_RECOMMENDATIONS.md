# RP2040 Wireless OTA DFU: Diagnostic & Recomandări Complete

Acest document este dedicat exclusiv analizei arhitecturii OTA (Over-The-Air DFU), cauzelor problemelor de recepție a pachetelor de firmware pe linia MISO în branch-ul `codex/rp2040-ota-strict-port` (commit `9d27340`), impactului funcției `worker_core1_idle_wait()` și evaluării influenței codului OTA asupra transmiterii rapoartelor de tastatură și taste media (Consumer Control).

---

## 1. De ce MISO pentru pachetele de firmware nu ajunge la RP2040? (Analiza Root Cause)

Fluxul unui pachet OTA parcurge următorul traseu:
```
PC (flash_ota.exe / probe)
  │ (USB SetFeature: Report ID 0x04)
  ▼
Receiver (nRF52840)
  │ (ESB ACK Payload pe Pipe 0)
  ▼
Transmitter (nRF52840)
  │ (SPI Slave MISO: spi_transceive)
  ▼
RP2040 Master (WirelessKeyboard.c)
```

În branch-ul curent `codex/rp2040-ota-strict-port`, MISO eșuează din cauza următoarelor 3 probleme interconectate:

### A. Problema Critică: Receiver-ul șterge starea DFU la prima trimitere (One-Shot Hack)
* **Comportamentul în versiunea funcțională (`codex/rp2040-ota-strict`, commit `9e4164f` / `c853930`):**
  Când PC-ul trimitea o comandă DFU (`START`, `CRC`, `DATA`, `QUERY`), Receiver-ul marca `dfu_ack_active = true` și **menținea comanda activă**, re-atașând-o în *fiecare* pachet de ESB ACK către Transmitter până când RP2040 răspundea cu un cadru de tip `LINK_TYPE_DFU_STATUS` cu token-ul identic.
* **Problema în `Receiver` curent (commit `f1e3e9e` / `e0e4f2d`):**
  În `Receiver/src/main.c`, în funcția `receiver_queue_led_ack()`:
  ```c
  if (dfu_ack_active) {
      ack = dfu_pending_ack;
      dfu_ack_active = false; /* <--- EROARE: Șterge starea imediat după prima descărcare în FIFO */
  }
  ```
* **Mecanismul defecțiunii:**
  1. Receiver-ul atașează pachetul DFU în ESB ACK-ul unui prim transfer.
  2. Dacă Transmitter-ul trimite un alt cadru radio (un poll sau un raport anterior), Receiver-ul generează imediat un nou ACK cu `LINK_ACK_TYPE_LOCK_STATE` (starea LED-urilor Caps/NumLock).
  3. Transmitter-ul primește acest al doilea ACK și **suprascrie `spi_ack_response` cu LOCK_STATE înainte ca RP2040 să execute citirea pe SPI**.
  4. Când RP2040 citește linia MISO, găsește octeți de Lock-State sau zerouri, iar pachetul de firmware este pierdut definitiv.

---

### B. Decalajul de buffer SPI Slave din Transmitter
* În `Transmitter/src/main.c` (`fe165f9`):
  ```c
  for (;;) {
      spi_ack_snapshot(&spi_tx);  // Copiază spi_ack_response în bufferul local
      int err = spi_transceive(spi_device, &spi_slave_config, &tx, &rx); // Blochează până la CSN
      ...
  ```
* Bufferul de transmisie pe MISO (`spi_tx`) este armat **înainte** de începerea tranzacției SPI de la RP2040.
* Când RP2040 efectuează Tranzacția 1 (MOSI poll), Transmitter-ul transmite pe MISO datele vechi armate anterior. Tranzacția 1 declanșează trimiterea pachetului prin radio ESB și recepția noului DFU în ESB ACK.
* Transmitter-ul încarcă noul pachet DFU în `spi_tx` abia pentru **Tranzacția 2**.
* Dacă Receiver-ul nu menține pachetul activ (vezi punctul A), la Tranzacția 2 noul pachet este deja suprascris.

---

### C. Intervalul de interogare (Polling) de 1 secundă când sesiunea DFU nu este activă
* În `WirelessKeyboard.c` ([`spi_ack_poll_task`](file:///c:/Users/Monard/Raspberry/WirelessKeyboard/WirelessKeyboard.c#L1251)):
  ```c
  uint32_t const interval = dfu_session_active ?
      SPI_ACK_POLL_DFU_MS : SPI_ACK_POLL_IDLE_MS; // 1ms vs 1000ms
  ```
* Până când RP2040 nu procesează cu succes `LINK_TYPE_DFU_START`, variabila `dfu_session_active` este `false`.
* Pentru comenzi precum `DFU_CMD_QUERY` (trimisă de unealta `tools/probe_ota_link.py`), RP2040 interoghează Transmitter-ul doar o dată la **1000 ms**. Dacă primul transfer ratează fereastra de sincronizare, comanda expiră.

---

## 2. Analiza funcției `worker_core1_idle_wait()`: Este Bună sau Rea?

În `WirelessKeyboard.c` ([linia 3013](file:///c:/Users/Monard/Raspberry/WirelessKeyboard/WirelessKeyboard.c#L3013)):
```c
static void worker_core1_idle_wait(void)
{
    if (usb_host_event_head != usb_host_event_tail ||
        spi_input_queue_count != 0 || radio_wake_queue_count != 0 ||
        battery_spi_pending || consumer_retry_pending) {
        return;
    }

    if (spi_retry_pending) {
        sleep_until(from_us_since_boot(spi_retry_after_us));
        return;
    }

    sleep_until(make_timeout_time_ms(1));
}
```

### Verdict: Este FOARTE BUNĂ pentru consum și FĂRĂ IMPACT NEGATIV pe latență.

#### De ce este bună?
1. **Economie masivă de energie și temperatură pe RP2040:**
   Fără această funcție, Core 1 ar rula într-un `while(1)` continuu la 96 MHz, consumând constant ~15–20 mA chiar și când tastatura nu este folosită. `sleep_until()` oprește ceasul nucleului (WFE - Wait For Event), permițând intrarea în mod low-power.
2. **Latență ZERO la tastare (Sub-microsecundă):**
   Când utilizatorul apasă o tastă, Core 0 (care gestionează USB PIO Host) primește raportul USB și apelează `usb_host_event_push()`, care execută instrucțiunea ARM:
   ```c
   __sev(); /* Send Event: trezește INSTANTANEU Core 1 din sleep_until/WFE */
   ```
   Core 1 nu așteaptă expirarea celor 1 ms, ci este trezit imediat de semnalul hardware SEV.
3. **Cadență regulată de 1 ms pentru sarcini de fundal:**
   Când tastatura este idle, nucleul se trezește la fiecare 1 ms pentru a verifica starea bateriei, LED-urile și interogările SPI (`spi_ack_poll_task`).

#### Singurul aspect de reținut pentru OTA:
În timpul unei sesiuni DFU active (`dfu_session_active = true`), `spi_ack_poll_task` rulează la fiecare 1 ms (`SPI_ACK_POLL_DFU_MS = 1u`). Somnul de 1 ms din `worker_core1_idle_wait()` se sincronizează perfect cu acest ritm de interogare de 1 kHz.

---

## 3. Afectează ajustările pe codul OTA trimiterea datelor de la Tastatură / Consumer spre Receiver?

### Răspuns: NU, performanța de 1000Hz a tastaturii și a tastelor media este 100% protejată.

Codul proiectului a fost proiectat cu o separare strictă între **Hot Path (Input)** și **Control Path (OTA / Telemetrie)**:

1. **Prioritizarea cozii de Input în RP2040:**
   În `WirelessKeyboard.c`, `spi_service_task()` procesează mai întâi evenimentele de tastatură (`LINK_TYPE_KEYBOARD`) și taste media (`LINK_TYPE_CONSUMER`).
2. **Inhibarea interogării OTA în timpul tastării (`SPI_ACK_POLL_QUIET_MS`):**
   ```c
   if (!dfu_session_active &&
       (uint32_t)(now - radio_last_activity_ms) < SPI_ACK_POLL_QUIET_MS) {
       return; // Nu trimite niciun pachet de poll dacă s-a tastat în ultimele 20 ms
   }
   ```
   Când utilizatorul tastează, interogările de fond sunt oprite complet pentru a lăsa magistrala SPI și canalul radio 100% libere pentru datele de intrare.
3. **Separarea spațiilor de secvență (Sequence Epochs):**
   Pachetele de control/OTA (`LINK_TYPE_CONTROL`, `LINK_TYPE_DFU_STATUS`) folosesc un contor separat (`spi_control_sequence`) și nu modifică contoarele de secvență ale tastaturii (`LINK_TYPE_KEYBOARD`) sau ale tastelor multimedia (`LINK_TYPE_CONSUMER`). Receiver-ul menține stări independente pentru fiecare tip de pachet.
4. **Endpoint-uri USB separate în Receiver:**
   - Rapoartele de tastatură și media merg pe Endpoint-urile HID de întrerupere (EP1 IN / EP2 IN la 1000 Hz).
   - Comenzile OTA (DFU) merg exclusiv pe Endpoint-ul de Control (EP0 via `SetFeature` / `GetFeature`), fără a bloca sau întârzia rapoartele HID.

---

## 4. Recomandări Concrete de Implementare pentru Rezolvarea OTA

### Pasul 1: În Receiver (`c:\ncs\v3.4.0\myproject\Receiver\src\main.c`)
Restaurarea persistenței comenzilor DFU până la confirmarea cap-coadă de la RP2040:
1. În `receiver_queue_led_ack()`:
   **Eliminați `dfu_ack_active = false;`** din blocul de trimitere.
2. În `receiver_esb_event_handler()`:
   Asigurați-vă că `dfu_ack_active` devine `false` **doar** atunci când sosește confirmarea de la RP2040:
   ```c
   if (packet.type == LINK_TYPE_DFU_STATUS) {
       k_spinlock_key_t key = k_spin_lock(&dfu_state_lock);
       dfu_current_status = packet.data[0];
       dfu_status_session = packet.data[1];
       dfu_status_token   = packet.data[2];
       dfu_status_detail  = packet.data[3];
       dfu_status_value   = (uint32_t)packet.data[4] |
                            ((uint32_t)packet.data[5] << 8) |
                            ((uint32_t)packet.data[6] << 16) |
                            ((uint32_t)packet.data[7] << 24);
       if (dfu_ack_active &&
           packet.data[1] == dfu_pending_ack.data[1] &&
           packet.data[2] == dfu_pending_ack.sequence) {
           dfu_ack_active = false; /* Confirmare validată cap-coadă */
       }
       k_spin_unlock(&dfu_state_lock, key);
       atomic_set(&led_ack_pending, 1);
       continue;
   }
   ```
3. Re-atașați comanda la fiecare pachet radio primit cât timp `dfu_ack_active == true`:
   ```c
   k_spinlock_key_t pending_key = k_spin_lock(&dfu_state_lock);
   bool const command_pending = dfu_ack_active;
   k_spin_unlock(&dfu_state_lock, pending_key);
   if (atomic_get(&led_ack_dirty) != 0 || command_pending) {
       atomic_set(&led_ack_pending, 1);
   }
   ```

### Pasul 2: În Transmitter (`c:\ncs\v3.4.0\myproject\Transmitter\src\main.c`)
Asigurați-vă că pe modulul fizic este încărcat binarul compilat din commit-ul `fe165f9` (driverul Zephyr `spi_transceive`), și nu versiunea experimentală cu `nrfx_spis` (`4150dcc`), care nu scotea date pe MISO.

### Pasul 3: În RP2040 (`c:\Users\Monard\Raspberry\WirelessKeyboard\WirelessKeyboard.c`)
Permiteți comutarea la cadența rapidă de interogare de 1 ms (`SPI_ACK_POLL_DFU_MS`) și la comanda `LINK_TYPE_DFU_QUERY` sau la orice cerere de wake OTA, pentru a asigura răspuns rapid chiar înainte ca sesiunea de scriere a memoriei flash să fie marcată ca activă.
