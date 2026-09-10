import os
import heapq

class SchematicRouter:
    def __init__(self, width=2200, height=1400, grid_size=20):
        self.w = width
        self.h = height
        self.grid = grid_size
        self.obstacles = []
        self.used_nodes = {}  # dict de tip {(x,y): "nume_retea"}
        self.pins = set()
        
        self.svg_boxes = []
        self.svg_pins = []
        self.svg_wires = []
        self.svg_texts = []
        
        # Desenăm pattern-ul pentru grila vizuală (opțional, arată ca un soft EDA profesionist)
        self.svg_boxes.append('''
        <defs>
            <pattern id="grid" width="20" height="20" patternUnits="userSpaceOnUse">
                <circle cx="2" cy="2" r="1" fill="#cbd5e1"/>
            </pattern>
        </defs>
        <rect width="100%" height="100%" fill="url(#grid)" />
        ''')

    def add_box(self, x, y, w, h, title, subtitle="", rx=12, fill="#f8fafc", stroke="#475569", dash=None):
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        # Adăugăm componenta ca obstacol
        self.obstacles.append((x, y, x+w, y+h))
        
        self.svg_boxes.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="4"{dash_attr}/>')
        self.svg_texts.append(f'<text x="{x+w//2}" y="{y+40}" text-anchor="middle" font-family="Segoe UI" font-size="24" font-weight="bold" fill="#1e293b">{title}</text>')
        if subtitle:
            self.svg_texts.append(f'<text x="{x+w//2}" y="{y+70}" text-anchor="middle" font-family="Segoe UI" font-size="18" fill="#334155">{subtitle}</text>')

    def add_pin(self, x, y, label, is_left=True, color="#334155"):
        self.pins.add((x, y))
        self.svg_pins.append(f'<circle cx="{x}" cy="{y}" r="6" fill="#ffffff" stroke="{color}" stroke-width="3"/>')
        anchor = "end" if is_left else "start"
        offset = -12 if is_left else 12
        
        # Font mărit la 18px și coordonata Y urcată cu 10px (y-5) pentru a sta clar deasupra firului
        self.svg_texts.append(f'<text x="{x+offset}" y="{y-5}" text-anchor="{anchor}" font-family="Consolas" font-size="18" font-weight="bold" fill="{color}">{label}</text>')
        return (x, y)

    def is_obstacle(self, x, y, target_nodes):
        if (x, y) in target_nodes:
            return False
        # Pinii străini sunt interziși pentru a nu trece cu firul peste ei
        if (x, y) in self.pins:
            return True
            
        # Dilatăm obstacolul cu o unitate de grilă pentru a preveni lipirea firelor de marginile componentelor
        for (x1, y1, x2, y2) in self.obstacles:
            if x1 - self.grid < x < x2 + self.grid and y1 - self.grid < y < y2 + self.grid:
                return True
        return False

    def route_wire(self, start, end, color="#0f172a", net_name=None, width=6, overlay=None):
        open_set = []
        heapq.heappush(open_set, (0, start, [start], (0, 0)))
        visited = set()
        
        target_nodes = set()
        if isinstance(end, str):
            # Caută orice punct aparținând rețelei (net_name) cerute
            for node, name in self.used_nodes.items():
                if name == end:
                    target_nodes.add(node)
            if not target_nodes:
                print(f"Eroare: Rețeaua {end} nu există încă!")
                return False
        else:
            target_nodes.add(end)
            
        while open_set:
            cost, curr, path, last_dir = heapq.heappop(open_set)
            
            if curr in target_nodes:
                # Traseu găsit! Marcăm toate nodurile ca fiind ocupate.
                for p in path:
                    if p != start and p not in target_nodes:
                        self.used_nodes[p] = net_name
                
                # Construim SVG Path
                d = f"M {path[0][0]} {path[0][1]}"
                for p in path[1:]:
                    d += f" L {p[0]} {p[1]}"
                
                self.svg_wires.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linejoin="round"/>')
                if overlay:
                    self.svg_wires.append(f'<path d="{d}" fill="none" stroke="{overlay}" stroke-width="{width-4}" stroke-linejoin="round"/>')
                
                # Dacă ne-am conectat la un nod care nu este un pin (joncțiune în T), desenăm un punct de cositorire
                if curr not in self.pins and isinstance(end, str):
                    self.svg_pins.append(f'<circle cx="{curr[0]}" cy="{curr[1]}" r="7" fill="{color}"/>')
                return True
                
            if curr in visited:
                continue
            visited.add(curr)
            
            # Verifică direcțiile ortogonale
            for dx, dy in [(0, self.grid), (0, -self.grid), (self.grid, 0), (-self.grid, 0)]:
                nx, ny = curr[0] + dx, curr[1] + dy
                
                if not (0 <= nx <= self.w and 0 <= ny <= self.h):
                    continue
                    
                if self.is_obstacle(nx, ny, target_nodes) and not (nx, ny) == start:
                    continue
                    
                # Verificăm dacă grila este deja folosită de alt fir
                if (nx, ny) in self.used_nodes and (nx, ny) not in target_nodes:
                    if net_name is None or self.used_nodes[(nx, ny)] != net_name:
                        continue
                        
                # Penalizăm curbele (vrem linii drepte și curate)
                turn_penalty = 0
                if last_dir != (0, 0) and last_dir != (dx, dy):
                    turn_penalty = 30
                    
                new_cost = cost + 10 + turn_penalty
                h = min(abs(nx - tx) + abs(ny - ty) for tx, ty in target_nodes)
                
                heapq.heappush(open_set, (new_cost + h, (nx, ny), path + [(nx, ny)], (dx, dy)))
                
        print(f"Avertisment: Nu s-a putut găsi traseu pentru {net_name} de la {start} la {end}")
        return False

    def build_svg(self, filename):
        svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.w}" height="{self.h}" viewBox="0 0 {self.w} {self.h}" style="background-color: #ffffff;">\n'
        
        # Ordine layere (ca în Photoshop): Cutii, Fire, Pini, Text
        svg += "\n".join(self.svg_boxes) + "\n"
        svg += "\n".join(self.svg_wires) + "\n"
        svg += "\n".join(self.svg_pins) + "\n"
        svg += "\n".join(self.svg_texts) + "\n"
        
        # Legendă
        svg += '''
        <text x="1100" y="80" text-anchor="middle" font-family="Segoe UI" font-size="42" font-weight="bold" fill="#0f172a" letter-spacing="1px">WIRELESS KEYBOARD WIRING SCHEMATIC (AUTO-ROUTED)</text>
        <rect x="50" y="1250" width="2100" height="120" rx="10" fill="#f8fafc" stroke="#334155" stroke-width="3"/>
        <text x="80" y="1290" font-family="Segoe UI" font-weight="bold" font-size="20">LEGENDĂ &amp; NOTE DE ASAMBLARE:</text>
        <text x="80" y="1320" font-family="Segoe UI" font-size="16">1. Traseele au fost generate folosind algoritmul A* pe o matrice NxN, evitând strict suprapunerea firelor.</text>
        <text x="80" y="1345" font-family="Segoe UI" font-size="16">2. Pinii SPI0 (GP16-GP19) sunt aliniați perfect orizontal către destinație pentru a facilita o lipire paralelă (zero încrucișări).</text>
        '''
        svg += '</svg>'
        
        with open(filename, "w", encoding="utf-8") as f:
            f.write(svg)
        print(f"Success: {filename} a fost generat!")

if __name__ == "__main__":
    router = SchematicRouter(2200, 1400, 20)

    # 1. AMPLASARE COMPONENTE
    router.add_box(100, 300, 160, 280, "BATTERY", "LiPo 1S")
    router.add_box(400, 300, 200, 280, "J5019", "Boost Reg.")
    router.add_box(740, 200, 120, 80, "SPST")
    
    router.add_box(400, 700, 200, 180, "USB-C", "Host Port")
    router.add_box(400, 920, 200, 160, "RGB LED", "Common Cathode")
    
    router.add_box(900, 300, 300, 800, "RP2040", "WeAct Studio", rx=16, stroke="#0f172a")
    router.add_box(1500, 460, 240, 440, "Nice!Nano", "nRF52840", rx=16, stroke="#0f172a")
    router.add_box(1900, 500, 160, 160, "DONGLE", "Receiver", dash="12 8")

    # 2. DEFINIRE PINI (Poziționați pentru aliniere perfectă)
    # Power & Switch
    bat_p      = router.add_pin(260, 400, "BAT+", False)
    bat_n      = router.add_pin(260, 440, "BAT-", False)
    boost_in_p = router.add_pin(400, 400, "BAT+", True)
    boost_in_n = router.add_pin(400, 440, "BAT-", True)
    boost_out_p= router.add_pin(600, 400, "VOUT+", False)
    boost_out_n= router.add_pin(600, 440, "VOUT-", False)
    sw_in      = router.add_pin(740, 240, "IN", True)
    sw_out     = router.add_pin(860, 240, "OUT", False)

    # USB
    usb_5v  = router.add_pin(600, 740, "5V", False)
    usb_dp  = router.add_pin(600, 780, "D+", False)
    usb_dn  = router.add_pin(600, 820, "D-", False)
    usb_gnd = router.add_pin(600, 860, "GND", False)

    # LED
    led_r   = router.add_pin(600, 940, "R", False)
    led_g   = router.add_pin(600, 980, "G", False)
    led_b   = router.add_pin(600, 1020, "B", False)
    led_gnd = router.add_pin(600, 1060, "GND", False)

    # RP2040 Stânga (Aliniați orizontal la destinații)
    rp_gp4  = router.add_pin(900, 780, "GP4", True)  # Aliniat cu USB D+
    rp_gp5  = router.add_pin(900, 820, "GP5", True)  # Aliniat cu USB D-
    rp_gnd1 = router.add_pin(900, 860, "GND", True)
    rp_gp7  = router.add_pin(900, 940, "GP7", True)  # Aliniat cu LED R
    rp_gp8  = router.add_pin(900, 980, "GP8", True)
    rp_gp9  = router.add_pin(900, 1020, "GP9", True)
    #rp_gnd2 = router.add_pin(900, 1060, "GND", True)

    # RP2040 Dreapta
    rp_vsys = router.add_pin(1200, 440, "VSYS", False)
    #rp_gnd3 = router.add_pin(1200, 480, "GND", False)
    rp_3v3  = router.add_pin(1200, 520, "3V3", False)
    rp_gp28 = router.add_pin(1200, 600, "GP28", False)
    rp_gp19 = router.add_pin(1200, 720, "GP19", False)
    rp_gp18 = router.add_pin(1200, 760, "GP18", False)
    rp_gp17 = router.add_pin(1200, 800, "GP17", False)
    rp_gp16 = router.add_pin(1200, 840, "GP16", False)

    # Nice!Nano
    nn_vcc  = router.add_pin(1500, 520, "VCC", True)  # Aliniat cu RP2040 3V3
    nn_mosi = router.add_pin(1500, 720, "P0.20", True) # Aliniat cu GP19
    nn_sck  = router.add_pin(1500, 760, "P0.17", True)
    nn_csn  = router.add_pin(1500, 800, "P0.22", True)
    nn_miso = router.add_pin(1500, 840, "P0.08", True)
    nn_gnd  = router.add_pin(1500, 940, "GND", True)
    

    # 3. SETĂM MAGISTRALA PRINCIPALĂ (GROUND BUS) LA BAZĂ
    # Algoritmul A* va lega toate GND-urile la cel mai apropiat punct al acestei bare
    GND_BUS_Y = 1160
    for x in range(200, 2001, 20):
        router.used_nodes[(x, GND_BUS_Y)] = "GND"
    router.svg_wires.append(f'<line x1="200" y1="{GND_BUS_Y}" x2="2000" y2="{GND_BUS_Y}" stroke="#0f172a" stroke-width="12" stroke-linecap="round"/>')
    router.svg_texts.append(f'<text x="1100" y="{GND_BUS_Y + 30}" text-anchor="middle" font-family="Segoe UI" font-weight="bold" font-size="16" fill="#0f172a">COMMON GROUND BUS</text>')

    # 4. TRASĂM FIRELE (Rezolvare topologică prin rute de evitare)
    
    # Alimentare Stânga
    router.route_wire(bat_p, boost_in_p, "#ef4444", "V_BAT")
    router.route_wire(boost_out_p, sw_in, "#ef4444", "V_BOOST")
    
    # === REZOLVARE CONFLICTE DREAPTA (Ocolire pe sus) ===
    # Trimitem VSYS, GND și ADC în sus pe culoare paralele (x=1220, 1240, 1260) 
    # pe deasupra plăcii (y=240, 220, 200) pentru a nu bloca pinii SPI și 3V3.
    
    # 1. V_SYS (Culoarul x=1220)
    router.route_wire(rp_vsys, (1220, 440), "#ef4444", "V_SYS")
    router.route_wire((1220, 440), (1220, 240), "#ef4444", "V_SYS")
    router.route_wire((1220, 240), sw_out, "#ef4444", "V_SYS")
    router.route_wire(usb_5v, "V_SYS", "#ef4444", "V_SYS")
    

    # 3. ADC (Culoarul x=1260)
    router.route_wire(rp_gp28, (1260, 600), "#f97316", "ADC")
    router.route_wire((1260, 600), (1260, 200), "#f97316", "ADC")
    router.route_wire((1260, 200), (380, 200), "#f97316", "ADC")
    router.route_wire((380, 200), "V_BAT", "#f97316", "ADC")

    # === CALE LIBERĂ ===
    # Acum liniile orizontale sunt 100% libere să treacă direct spre Nice!Nano!
    router.route_wire(rp_3v3, nn_vcc, "#000000", "3V3", width=12, overlay="#ffffff")

    router.route_wire(rp_gp19, nn_mosi, "#3b82f6", "SPI_MOSI")
    router.route_wire(rp_gp18, nn_sck, "#eab308", "SPI_SCK")
    router.route_wire(rp_gp17, nn_csn, "#22c55e", "SPI_CSN")
    router.route_wire(rp_gp16, nn_miso, "#a855f7", "SPI_MISO")

    # USB Data (Stânga)
    router.route_wire(rp_gp4, usb_dp, "#8b5a2b", "USB_DP")
    router.route_wire(rp_gp5, usb_dn, "#64748b", "USB_DN")

    # RGB LED (Stânga)
    router.route_wire(rp_gp7, led_r, "#ef4444", "LED_R")
    router.route_wire(rp_gp8, led_g, "#22c55e", "LED_G")
    router.route_wire(rp_gp9, led_b, "#3b82f6", "LED_B")

    # Restul de conexiuni GND (Curată liber în jos spre magistrală)
    router.route_wire(bat_n, "GND", "#0f172a", "GND")
    router.route_wire(boost_in_n, "GND", "#0f172a", "GND")
    router.route_wire(boost_out_n, "GND", "#0f172a", "GND")
    router.route_wire(usb_gnd, "GND", "#0f172a", "GND")
    router.route_wire(led_gnd, "GND", "#0f172a", "GND")
    router.route_wire(rp_gnd1, "GND", "#0f172a", "GND")
    #router.route_wire(rp_gnd2, "GND", "#0f172a", "GND")
    router.route_wire(nn_gnd, "GND", "#0f172a", "GND")
    
    router.build_svg("Wiring_Schematic_Auto.svg")