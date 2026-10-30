// ============================================================================
//  FM-1 Transfert : envoie des morceaux (.A2M...) au tracker du FM-1, par USB,
//  et en recupere (l'EXPORT A2M du tracker, pour AdLib Tracker II et Amber).
//
//  Le protocole est celui de firmware/compat/fm1_transfert.c : du SysEx,
//  F0 pack7(00 'T' 'K' cmd ...) F7 ; le FM-1 repond a chaque message, et on
//  attend sa reponse avant d'envoyer le suivant.
//  Compiler : tracker/mac/build_app.sh  ->  tracker/mac/build/FM-1 Transfert.app
// ============================================================================
import AppKit
import CoreMIDI
import SwiftUI
import UniformTypeIdentifiers

// ── Le format des messages ───────────────────────────────────────────────────
enum Protocole {
    static let entete: [UInt8] = [0x00, 0x54, 0x4B]   // 00 'T' 'K'
    static let bonjour: UInt8 = 0x00, debut: UInt8 = 0x01, donnees: UInt8 = 0x02, fin: UInt8 = 0x03, annule: UInt8 = 0x04
    static let liste: UInt8 = 0x05, ouvre: UInt8 = 0x06, lit: UInt8 = 0x07, ferme: UInt8 = 0x08   // (recuperer)
    static let morceauMax = 512                       // octets de fichier par message

    /// 8 bits -> 7, poids faibles d'abord (ota_pack7 du firmware)
    static func pack7(_ entree: [UInt8]) -> [UInt8] {
        var sortie: [UInt8] = []
        var acc: UInt32 = 0, nb: UInt32 = 0
        for o in entree {
            acc |= UInt32(o) << nb
            nb += 8
            while nb >= 7 {
                sortie.append(UInt8(acc & 0x7F))
                acc >>= 7
                nb -= 7
            }
        }
        if nb > 0 { sortie.append(UInt8(acc & 0x7F)) }
        return sortie
    }

    /// 7 bits -> 8 (ota_unpack7 du firmware)
    static func unpack7(_ entree: [UInt8]) -> [UInt8] {
        var sortie: [UInt8] = []
        var acc: UInt32 = 0, nb: UInt32 = 0
        for o in entree {
            acc |= UInt32(o & 0x7F) << nb
            nb += 7
            if nb >= 8 {
                sortie.append(UInt8(acc & 0xFF))
                acc >>= 8
                nb -= 8
            }
        }
        return sortie
    }

    static func trame(_ message: [UInt8]) -> [UInt8] { [0xF0] + pack7(message) + [0xF7] }

    static func u32(_ v: UInt32) -> [UInt8] { [UInt8(v & 0xFF), UInt8(v >> 8 & 0xFF), UInt8(v >> 16 & 0xFF), UInt8(v >> 24 & 0xFF)] }

    /// le CRC-32 de zlib, comme tk_crc32 du firmware
    static func crc32(_ donnees: [UInt8]) -> UInt32 {
        var c: UInt32 = 0xFFFF_FFFF
        for o in donnees {
            c ^= UInt32(o)
            for _ in 0..<8 { c = (c >> 1) ^ (0xEDB8_8320 & (0 &- (c & 1))) }
        }
        return ~c
    }

    /// Le nom sur le FM-1 : en majuscules, lettres, chiffres, espace - _ . (40 au plus)
    static func nomFM1(_ url: URL) -> String {
        let permis = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.")
        let nom = String(url.lastPathComponent.uppercased().map { permis.contains($0) ? $0 : "_" })
        return String(nom.prefix(40))
    }
}

// ── Le lien MIDI avec le FM-1 ────────────────────────────────────────────────
final class Lien: ObservableObject, @unchecked Sendable {
    @Published var etat = "Recherche du FM-1…"
    @Published var connecte = false

    private var client = MIDIClientRef()
    private var sortie = MIDIPortRef()
    private var entree = MIDIPortRef()
    private var destination: MIDIEndpointRef = 0
    private var sysex: [UInt8] = []                   // la trame en cours de reception
    private let verrou = NSLock()
    private var attente: (numero: Int, suite: CheckedContinuation<[UInt8]?, Never>)?
    private var numero = 0

    init() {
        MIDIClientCreateWithBlock("FM-1 Transfert" as CFString, &client) { [weak self] notif in
            if notif.pointee.messageID == .msgSetupChanged {   // un appareil branche ou debranche
                Task { await self?.cherche() }
            }
        }
        MIDIOutputPortCreate(client, "sortie" as CFString, &sortie)
        MIDIInputPortCreateWithBlock(client, "entree" as CFString, &entree) { [weak self] liste, _ in
            self?.recoit(liste)
        }
    }

    private func publie(_ texte: String, _ ok: Bool) {
        DispatchQueue.main.async {
            self.etat = texte
            self.connecte = ok
        }
    }

    private static func nom(_ objet: MIDIObjectRef) -> String {
        var nom: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(objet, kMIDIPropertyDisplayName, &nom)
        return (nom?.takeRetainedValue() as String?) ?? ""
    }

    /// Trouve le FM-1 : on demande « bonjour » a chaque sortie MIDI, celle qui repond est la bonne.
    func cherche() async {
        publie("Recherche du FM-1…", false)
        for k in 0..<MIDIGetNumberOfSources() {
            MIDIPortConnectSource(entree, MIDIGetSource(k), nil)
        }
        var candidats: [MIDIEndpointRef] = []
        for k in 0..<MIDIGetNumberOfDestinations() { candidats.append(MIDIGetDestination(k)) }
        // (d'abord celles dont le nom ressemble au FM-1)
        candidats.sort { a, b in
            let pa = ["FELUCCA", "FM-1", "FM1", "OPL3"].contains { Lien.nom(a).uppercased().contains($0) }
            let pb = ["FELUCCA", "FM-1", "FM1", "OPL3"].contains { Lien.nom(b).uppercased().contains($0) }
            return pa && !pb
        }
        for d in candidats {
            destination = d
            if let r = await demande(Protocole.entete + [Protocole.bonjour], delai: 0.8), r.count >= 5, r[4] == 0 {
                let id = String(decoding: r.dropFirst(5), as: UTF8.self)
                publie("FM-1 connecté (\(id))", true)
                return
            }
        }
        destination = 0
        publie("FM-1 introuvable : branche-le en USB, avec le firmware du tracker.", false)
    }

    // Envoi d'une trame SysEx entiere
    private func envoie(_ octets: [UInt8]) {
        guard destination != 0 else { return }
        let taille = octets.count + 1024
        let memoire = UnsafeMutableRawPointer.allocate(byteCount: taille, alignment: 8)
        defer { memoire.deallocate() }
        let liste = memoire.bindMemory(to: MIDIPacketList.self, capacity: 1)
        var paquet = MIDIPacketListInit(liste)
        octets.withUnsafeBufferPointer { b in
            paquet = MIDIPacketListAdd(liste, taille, paquet, 0, octets.count, b.baseAddress!)
        }
        MIDISend(sortie, destination, liste)
    }

    // Reception : on assemble les trames SysEx, et une reponse 00 'T' 'K' reveille la demande en attente
    private func recoit(_ liste: UnsafePointer<MIDIPacketList>) {
        let decalage = MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
        for paquet in liste.unsafeSequence() {
            let n = Int(paquet.pointee.length)
            let octets = UnsafeRawPointer(paquet).advanced(by: decalage).assumingMemoryBound(to: UInt8.self)
            for i in 0..<n {
                let o = octets[i]
                if o == 0xF0 { sysex = [] ; continue }
                if o == 0xF7 {
                    let m = Protocole.unpack7(sysex)
                    sysex = []
                    if m.count >= 5, Array(m.prefix(3)) == Protocole.entete { reveille(m) }
                    continue
                }
                if o < 0x80 { sysex.append(o) } else if o < 0xF8 { sysex = [] }
            }
        }
    }

    private func reveille(_ m: [UInt8]?) {
        verrou.lock()
        let a = attente
        attente = nil
        verrou.unlock()
        a?.suite.resume(returning: m)
    }

    /// Envoie un message et attend la reponse du FM-1 (nil : pas de reponse a temps).
    func demande(_ message: [UInt8], delai: Double = 3) async -> [UInt8]? {
        await withCheckedContinuation { (suite: CheckedContinuation<[UInt8]?, Never>) in
            verrou.lock()
            numero += 1
            let n = numero
            attente = (n, suite)
            verrou.unlock()
            envoie(Protocole.trame(message))
            DispatchQueue.global().asyncAfter(deadline: .now() + delai) { [weak self] in
                guard let self else { return }
                self.verrou.lock()
                let a = self.attente
                if a?.numero == n { self.attente = nil }
                self.verrou.unlock()
                if a?.numero == n { a?.suite.resume(returning: nil) }
            }
        }
    }

    /// Une demande, refaite jusqu'a trois fois si le FM-1 ne repond pas.
    func demandeSure(_ message: [UInt8]) async -> [UInt8]? {
        for _ in 0..<3 {
            if let r = await demande(message) { return r }
        }
        return nil
    }
}

// ── Un fichier a envoyer ─────────────────────────────────────────────────────
struct Morceau: Identifiable {
    let id = UUID()
    let url: URL
    var etat = "prêt"
    var progres = 0.0
    var fini = false
    var erreur = false
}

// ── Un fichier du FM-1, a recuperer ──────────────────────────────────────────
struct Distant: Identifiable {
    let id = UUID()
    let nom: String
    let taille: Int
    var etat = ""
    var progres = 0.0
    var erreur = false
}

// ── La fenetre ───────────────────────────────────────────────────────────────
struct Fenetre: View {
    @StateObject private var lien = Lien()
    @State private var morceaux: [Morceau] = []
    @State private var envoiEnCours = false
    @State private var distants: [Distant] = []
    @State private var recupEnCours = false

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 8) {
                Circle().fill(lien.connecte ? Color.green : Color.red).frame(width: 10, height: 10)
                Text(lien.etat)
                Spacer()
                Button("Chercher le FM-1") { Task { await lien.cherche() } }
                    .disabled(envoiEnCours)
            }

            ZStack {
                RoundedRectangle(cornerRadius: 8)
                    .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6]))
                    .foregroundStyle(.secondary)
                if morceaux.isEmpty {
                    VStack(spacing: 6) {
                        Image(systemName: "music.note.list").font(.largeTitle).foregroundStyle(.secondary)
                        Text("Glisse ici tes morceaux .A2M").foregroundStyle(.secondary)
                    }
                } else {
                    List {
                        ForEach(morceaux) { m in
                            HStack {
                                Text(Protocole.nomFM1(m.url))
                                Spacer()
                                if !m.fini && m.progres > 0 {
                                    ProgressView(value: m.progres).frame(width: 120)
                                }
                                Text(m.etat)
                                    .foregroundStyle(m.erreur ? Color.red : m.fini ? Color.green : Color.secondary)
                                    .frame(width: 150, alignment: .trailing)
                            }
                        }
                    }
                    .padding(4)
                }
            }
            .frame(minHeight: 220)
            .onDrop(of: [UTType.fileURL], isTargeted: nil) { fournisseurs in
                for f in fournisseurs {
                    _ = f.loadObject(ofClass: URL.self) { url, _ in
                        if let url { DispatchQueue.main.async { ajoute([url]) } }
                    }
                }
                return true
            }

            HStack {
                Button("Ajouter des morceaux…") { choisit() }
                Button("Vider la liste") { morceaux.removeAll() }
                    .disabled(morceaux.isEmpty || envoiEnCours)
                Spacer()
                Button("Envoyer au FM-1") { Task { await envoieTout() } }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!lien.connecte || morceaux.isEmpty || envoiEnCours)
            }

            Text("Les morceaux arrivent dans PROJECT > LOAD. Arrête la lecture sur le FM-1 avant d'envoyer : l'écriture en mémoire peut faire sauter le son.")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Divider()

            // Récupérer : les morceaux du FM-1 (dont ceux de PROJECT > EXPORT A2M), vers le Mac
            HStack {
                Text("Sur le FM-1").font(.headline)
                Spacer()
                Button("Lire la liste") { Task { await litListe() } }
                    .disabled(!lien.connecte || envoiEnCours || recupEnCours)
            }
            List {
                ForEach(distants) { d in
                    HStack {
                        Text(d.nom)
                        Text("\(max(1, (d.taille + 1023) / 1024)) Ko").foregroundStyle(.secondary)
                        Spacer()
                        if d.progres > 0 && d.progres < 1 {
                            ProgressView(value: d.progres).frame(width: 100)
                        }
                        Text(d.etat).foregroundStyle(d.erreur ? Color.red : Color.secondary)
                        Button("Récupérer…") { Task { await recupere(d) } }
                            .disabled(!lien.connecte || envoiEnCours || recupEnCours)
                    }
                }
            }
            .frame(minHeight: 140)

            Text("Pour ouvrir un morceau dans AdLib Tracker II ou Amber : sur le FM-1, PROJECT > EXPORT A2M, puis « Lire la liste » et « Récupérer… ».")
                .font(.footnote)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(18)
        .frame(width: 560)
        .task { await lien.cherche() }
    }

    private func ajoute(_ urls: [URL]) {
        for u in urls where !morceaux.contains(where: { $0.url == u }) {
            morceaux.append(Morceau(url: u))
        }
    }

    private func choisit() {
        let panneau = NSOpenPanel()
        panneau.allowsMultipleSelection = true
        panneau.canChooseDirectories = false
        panneau.allowedContentTypes = [UTType(filenameExtension: "a2m") ?? .data, UTType(filenameExtension: "obt") ?? .data]
        panneau.message = "Choisis les morceaux à envoyer au FM-1"
        if panneau.runModal() == .OK { ajoute(panneau.urls) }
    }

    private func changeDistant(_ id: UUID, _ f: (inout Distant) -> Void) {
        if let k = distants.firstIndex(where: { $0.id == id }) { f(&distants[k]) }
    }

    private static func u32(_ r: [UInt8], _ i: Int) -> Int {
        Int(r[i]) | Int(r[i + 1]) << 8 | Int(r[i + 2]) << 16 | Int(r[i + 3]) << 24
    }

    /// La liste des morceaux du FM-1, un par un (LISTE n), jusqu'a ce qu'il n'y en ait plus.
    @MainActor
    private func litListe() async {
        recupEnCours = true
        defer { recupEnCours = false }
        var liste: [Distant] = []
        for k in 0..<255 {
            guard let r = await lien.demandeSure(Protocole.entete + [Protocole.liste, UInt8(k)]),
                  r[3] == (Protocole.liste | 0x40), r[4] == 0, r.count >= 9 else { break }
            liste.append(Distant(nom: String(decoding: r[9...], as: UTF8.self), taille: Fenetre.u32(r, 5)))
        }
        distants = liste
    }

    /// Recupere un morceau (OUVRE, LIT par 256 octets, FERME), puis demande ou l'enregistrer.
    @MainActor
    private func recupere(_ d: Distant) async {
        recupEnCours = true
        defer { recupEnCours = false }
        let id = d.id
        func echec(_ texte: String) { changeDistant(id) { $0.etat = texte; $0.erreur = true; $0.progres = 0 } }
        changeDistant(id) { $0.etat = "lecture…"; $0.erreur = false; $0.progres = 0.001 }
        guard let r = await lien.demandeSure(Protocole.entete + [Protocole.ouvre] + Array(d.nom.utf8) + [0]),
              r[3] == (Protocole.ouvre | 0x40) else { return echec("pas de réponse") }
        guard r[4] == 0, r.count >= 9 else { return echec("introuvable") }
        let taille = Fenetre.u32(r, 5)
        var donnees: [UInt8] = []
        while donnees.count < taille {
            guard let r = await lien.demandeSure(Protocole.entete + [Protocole.lit] + Protocole.u32(UInt32(donnees.count))),
                  r[3] == (Protocole.lit | 0x40), r[4] == 0, r.count >= 9, Fenetre.u32(r, 5) == donnees.count else {
                _ = await lien.demande(Protocole.entete + [Protocole.ferme])
                return echec("pas de réponse")
            }
            if r.count == 9 { break }             // (plus rien : le fichier est plus court qu'annonce)
            donnees += r[9...]
            changeDistant(id) { $0.progres = Double(donnees.count) / Double(max(1, taille)) }
        }
        _ = await lien.demande(Protocole.entete + [Protocole.ferme])
        guard donnees.count == taille else { return echec("incomplet") }
        changeDistant(id) { $0.progres = 1 }
        let panneau = NSSavePanel()
        panneau.nameFieldStringValue = d.nom
        panneau.message = "Où enregistrer \(d.nom) ?"
        guard panneau.runModal() == .OK, let url = panneau.url else { return changeDistant(id) { $0.etat = "annulé" } }
        do {
            try Data(donnees).write(to: url)
            changeDistant(id) { $0.etat = "récupéré ✓" }
        } catch {
            echec("écriture impossible")
        }
    }

    private func change(_ id: UUID, _ f: (inout Morceau) -> Void) {
        if let k = morceaux.firstIndex(where: { $0.id == id }) { f(&morceaux[k]) }
    }

    @MainActor
    private func envoieTout() async {
        envoiEnCours = true
        defer { envoiEnCours = false }
        for m in morceaux where !m.fini {
            await envoie(m)
        }
    }

    @MainActor
    private func envoie(_ m: Morceau) async {
        let id = m.id
        func echec(_ texte: String) { change(id) { $0.etat = texte; $0.erreur = true; $0.progres = 0 } }
        guard let donnees = try? [UInt8](Data(contentsOf: m.url)), !donnees.isEmpty else { return echec("illisible") }
        change(id) { $0.etat = "envoi…"; $0.erreur = false; $0.progres = 0.001 }
        let nom = Array(Protocole.nomFM1(m.url).utf8)
        guard let r = await lien.demandeSure(Protocole.entete + [Protocole.debut] + Protocole.u32(UInt32(donnees.count)) + nom + [0]),
              r[3] == (Protocole.debut | 0x40) else { return echec("pas de réponse") }
        guard r[4] == 0 else { return echec("refusé (place ?)") }
        var position = 0
        while position < donnees.count {
            let fin = min(position + Protocole.morceauMax, donnees.count)
            let message = Protocole.entete + [Protocole.donnees] + Protocole.u32(UInt32(position)) + Array(donnees[position..<fin])
            guard let r = await lien.demandeSure(message), r[3] == (Protocole.donnees | 0x40) else {
                _ = await lien.demande(Protocole.entete + [Protocole.annule])
                return echec("pas de réponse")
            }
            switch r[4] {
            case 0: position = fin
            case 2 where r.count >= 9:            // le FM-1 attend une autre position : on reprend la
                position = Int(r[5]) | Int(r[6]) << 8 | Int(r[7]) << 16 | Int(r[8]) << 24
            default: return echec("mémoire pleine")
            }
            change(id) { $0.progres = Double(position) / Double(donnees.count) }
        }
        guard let r2 = await lien.demandeSure(Protocole.entete + [Protocole.fin] + Protocole.u32(Protocole.crc32(donnees))),
              r2[3] == (Protocole.fin | 0x40) else { return echec("pas de réponse") }
        guard r2[4] == 0 else { return echec(r2[4] == 3 ? "erreur de transfert" : "non enregistré") }
        change(id) { $0.etat = "envoyé ✓"; $0.fini = true; $0.progres = 1 }
    }
}

@main
struct TransfertApp: App {
    var body: some Scene {
        WindowGroup("FM-1 Transfert") { Fenetre() }
            .windowResizability(.contentSize)
    }
}
