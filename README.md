# MyDisplay

**MyDisplay** est le nom du projet : un second écran Windows distant, développé par phases. L'objectif est un moniteur étendu reconnu par Windows, et non un miroir du bureau.

## Guide rapide

MyDisplay comprend trois parties : un pilote d'affichage indirect (IDD) qui crée l'écran virtuel sur le PC hôte, un serveur qui capture cette sortie et l'envoie en H.264 via TLS, et un client Windows qui vérifie le serveur, demande un code PIN de session et affiche le flux. Le client ne nécessite pas de pilote.

Le flux distant sécurisé est encore en **pré-alpha** et réservé aux essais sur un réseau local privé. La capture fait encore un readback CPU, le profil vidéo est fixé à 1920x1080/60 FPS et le débit cible à 50 Mbit/s. Le pilote d'exemple est signé pour test et n'est pas prêt à distribuer.

Pour un essai entre deux PC, suivre les sections **Phase 1**, **Test sur le PC Windows 10** et **Capture réelle vers le client** plus bas. En bref : préparer l'écran IDD et le certificat sur l'hôte, autoriser TCP 48001 uniquement depuis l'adresse privée du client, démarrer le serveur, puis ouvrir `DisplayClientSetup.exe` sur le client avec l'adresse, le nom Windows de l'hôte et le PIN affiché. Dans le viewer, `F` affiche les statistiques, `F11` active le plein écran et `Échap` le quitte.

## Faisabilité et architecture retenue

Le projet est faisable avec les API publiques Windows. Le composant déterminant est un pilote **Indirect Display Driver (IDD)** bâti avec **UMDF 2 et IddCx**. Il expose un adaptateur et un moniteur à Windows, annonce des modes (par exemple 1280x720 ou 1920x1080 à 30/60 Hz) et reçoit la swap chain Direct3D produite pour ce moniteur. Windows peut alors placer des fenêtres sur cet écran via Paramètres > Système > Affichage et Win+P > Étendre.

Un IDD est un pilote UMDF en espace utilisateur, pas un pilote d'affichage kernel-mode écrit par ce projet. IddCx et le sous-système graphique Windows assurent l'intégration WDDM. Aucun composant kernel-mode personnalisé n'est prévu. Le code de pilote doit rester conforme aux contraintes UMDF : pas de GDI, d'API de fenêtrage, OpenGL ou Vulkan dans le pilote.

### Chemin d'une image

1. Windows compose le bureau de l'écran virtuel comme celui de tout autre écran.
2. Le pilote reçoit la swap chain associée au moniteur dans `EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN`, prépare son périphérique D3D11 avec `IddCxSwapChainSetDevice`, puis acquiert les surfaces avec `IddCxSwapChainReleaseAndAcquireBuffer`.
3. Le pilote signale la fin du travail GPU avec `IddCxSwapChainFinishedProcessingFrame` et libère/acquiert la surface suivante. Le sample Microsoft fournit la boucle de traitement de référence. La première phase ne fait que valider l'écran et ce cycle, sans réseau ni encodage.
4. Le futur serveur transmettra au processus utilisateur les surfaces ou une ressource GPU partageable. L'encodage et le réseau restent hors du chemin critique du pilote. On ne recapture pas l'écran principal et Desktop Duplication n'est pas requis : il capture une sortie existante et n'est pas la source de l'écran virtuel.

L'affichage du curseur peut être fourni séparément de la surface par les callbacks curseur IddCx. Il faudra le vérifier avant de conclure que la capture reproduit intégralement le bureau.

### Encodage et client

Le choix initial est H.264 à faible latence via Media Foundation (MFT matériel si disponible, notamment Intel Quick Sync ; MFT logiciel en repli). D3D11, DXGI et le gestionnaire de périphérique D3D11 Media Foundation sont les API à évaluer pour limiter les copies. Le chemin zéro-copie n'est pas garanti : il dépend de la compatibilité entre la surface IddCx, l'adaptateur de rendu et le MFT. Les frames doivent être abandonnées plutôt que mises en file si l'encodeur prend du retard.

Sur Windows 10, le client décode avec le MFT H.264 matériel quand le système en fournit un, sinon avec le décodeur logiciel Media Foundation. Il rend la dernière frame décodée dans une fenêtre plein écran sans redimensionnements/copies superflus, avec D3D11/DXGI. Un ancien PC peut ne pas avoir de décodage matériel ; la résolution et le débit devront alors être ajustables.

### Transport, découverte et résilience

| Transport | Avantages | Limites pour la vidéo interactive |
| --- | --- | --- |
| TCP | Simple, fiable, chiffrable avec TLS | Une perte bloque les octets suivants (head-of-line blocking) ; les frames en retard s'accumulent |
| UDP | Pas de retransmission bloquante ; on peut jeter les frames anciennes | Perte/réordonnancement à traiter dans le framing et le décodeur |
| QUIC | Chiffrement et contrôle de congestion intégrés ; flux fiables séparés | Les datagrammes QUIC demandent une bibliothèque compatible sur les deux PC et ajoutent une dépendance |

Décision provisoire : contrôle et négociation sur TCP/TLS, vidéo H.264 sur UDP avec numéros de séquence, horodatage, assemblage borné, abandon des frames incomplètes/anciennes et demande de frame IDR après perte critique. Ce choix privilégie la latence sur la livraison de chaque paquet. Il sera validé sur le LAN avant de le figer ; QUIC DATAGRAM avec MsQuic est l'alternative si la gestion intégrée du chiffrement/congestion vaut la dépendance. Aucun transport ne peut empêcher une image dégradée lors d'une perte ; le client doit continuer à afficher la dernière image complète plutôt que se bloquer.

Découverte initiale par annonces UDP sur le réseau local, avec saisie manuelle `IP:port` en secours. Le broadcast ne traverse pas les sous-réseaux/VLAN. L'interface Ethernet sera privilégiée via le choix d'adresse/routage Windows quand elle offre un chemin vers le serveur ; Wi-Fi reste supporté. La sélection doit se faire par interface/adresse joignable, pas par supposition que tout Ethernet est plus rapide.

Avant d'accepter le flux, les pairs seront appairés par PIN, puis utiliseront une session authentifiée/chiffrée. La découverte n'enverra pas de secret. Le serveur écoutera sur le LAN privé seulement, avec règle de pare-feu explicite et sans redirection de port ni exposition Internet.

### Entrée, audio et performances

Le client peut capturer clavier/souris en user-mode et envoyer les événements au serveur ; une application serveur dans la session interactive peut les injecter avec `SendInput`. Cela ne fonctionne pas sur le bureau sécurisé/UAC et UIPI limite l'injection vers des applications de niveau d'intégrité supérieur. Le curseur système et sa position doivent être traités séparément si nécessaire. Ces entrées sont facultatives : le client de base est récepteur/afficheur.

Audio est hors du chemin vidéo. Une future capture WASAPI (loopback de l'application ciblée si supporté par la version Windows) pourra emprunter un canal distinct et être désactivée sans changer le pipeline vidéo.

Un IDD ne garantit pas un impact nul sur un jeu : le bureau étendu, la composition, le transfert mémoire et l'encodage consomment du GPU, de la mémoire et du réseau. En revanche, la swap chain dédiée évite de capturer l'écran principal. Pour réduire l'impact : limiter le framerate par le mode d'affichage (30/60 Hz), encoder uniquement à l'arrivée de nouvelles frames, conserver peu de buffers, privilégier le MFT matériel et mesurer la charge réelle sur le Dell en jeu. L'absence de changements peut réduire le travail, mais Windows/IDD peut renvoyer une frame statique périodiquement.

## Privilèges et signature

- Développer/compiler : Visual Studio avec charge de travail C++ et composants Windows Driver Kit (WDK) correspondant au SDK installé.
- Installer/enlever le pilote, créer le périphérique logiciel et configurer le pare-feu : élévation administrateur.
- Tester un pilote local non publié : machine de test dédiée, pilote signé pour test et mode de signature de test Windows ; Secure Boot/politiques de l'appareil peuvent empêcher ce mode. Ne pas désactiver les protections du PC principal utilisé quotidiennement sans comprendre le risque.
- Distribuer à d'autres machines : signature adaptée à la distribution Windows (soumission/attestation ou certification Microsoft selon le cas). Un certificat de test n'est pas une solution de production.
- Serveur/client ordinaires : user-mode, sans privilège administrateur permanent. Aucun driver sur le PC client.

## Phases et critères d'arrêt

1. **IDD** : compiler le sample officiel intégré, installer son package et lancer son application d'instanciation. Vérifier qu'un seul moniteur distinct apparaît, passe en mode Étendre et affiche le bureau étendu. Ne pas commencer la phase 2 avant ce résultat.
2. **Serveur/capture** : identifier le moniteur virtuel par son identifiant PnP et capturer uniquement sa sortie DXGI. Desktop Duplication est utilisé après création du vrai écran IDD, jamais sur l'écran principal ; valider ensuite la capture continue et les changements de modes.
3. **Protocole** : figer framing, horodatage, séquences, MTU/fragmentation et pertes sur réseau local. Tests avec pertes/jitter. Pas de flux 1080p non compressé sur le LAN.
4. **Client Windows 10** : connexion manuelle d'abord, décodage d'une séquence de test puis rendu plein écran, reconnectable.
5. **Encodage matériel** : H.264 Media Foundation, MFT matériel puis repli logiciel ; comparer latence, copies et consommation.
6. **Performances** : 30/60 FPS, suppression des files d'attente, mesures en jeu et sur Ethernet/Wi-Fi.
7. **Produit** : interface, métriques, découverte, PIN, chiffrement, pare-feu et reconnexion.

Les phases 3 et 4 utilisent d'abord une séquence/payload de test borné, pas des frames brutes haute résolution. Le vrai flux vidéo n'est activé qu'après l'encodage de la phase 5.

### Phase 1 : prérequis et premier test

Source officielle : [Microsoft Windows Driver Samples - IndirectDisplay](https://github.com/microsoft/Windows-driver-samples/tree/main/video/IndirectDisplay), copiée dans `Server/DisplayDriver/IddSample` avec sa licence. `IddSampleApp` crée un périphérique logiciel Windows avec `SwDeviceCreate`; il ne remplace pas l'installation préalable du package de pilote. Le sample fournit `Direct3DDevice`, `SwapChainProcessor` et `IndirectDeviceContext`.

Sur un PC de test Windows 11 :

1. Installer **Visual Studio 2026 Community** avec la charge de travail **Développement Desktop en C++**, puis le composant Visual Studio **Windows Driver Kit** et le SDK/WDK compatibles. Au 2026-10-02, Microsoft recommande WDK `10.0.28000.2526` avec Visual Studio 2026 et un SDK de build `28000`. Si tu choisis Visual Studio 2022, Microsoft indique WDK `10.0.26100.6584` ; les numéros de build SDK/WDK doivent correspondre. Le projet utilise le toolset `WindowsUserModeDriver10.0`.
2. Ouvrir un terminal développeur **en tant qu'administrateur**, se placer à la racine du workspace, puis lancer :

   ```powershell
   .\Installer\Test-Phase1.ps1
   ```

   Le script choisit MSBuild amd64 si disponible, génère la solution x64 Debug, puis vérifie que le certificat du catalogue correspond au `.cer` généré. S'il n'est pas déjà approuvé, il affiche son sujet/empreinte et demande de taper `TRUST` avant de l'ajouter aux magasins machine **Trusted Root** et **Trusted Publisher**. Il ajoute ensuite le package avec `pnputil` et lance `IddSampleApp`. Il ne change pas le mode de signature ni les paramètres de démarrage Windows. N'approuve que le certificat local affiché par ce build.
3. Laisser l'application ouverte, puis vérifier dans Gestionnaire de périphériques et Paramètres > Système > Affichage qu'un seul écran virtuel distinct apparaît. Choisir **Étendre** et sélectionner 1920x1080 à 60 Hz. Le sample annonce aussi un mode préféré 2560x1440 à 144 Hz ; ne pas garder ce mode pour le test de charge. Les modes 1280x720 et 30 Hz ne sont pas fournis par ce premier écran sample.
4. Revenir à la console et appuyer sur `X` pour retirer le moniteur. La phase est réussie seulement après apparition, utilisation en mode Étendre et retrait effectif de l'écran virtuel.

Si la compilation ou l'installation échoue, conserver le premier message d'erreur et vérifier la version du WDK/OS, la signature, Secure Boot et les journaux Code Integrity. Ne désactive pas les protections du PC principal quotidien ; utilise un PC de test.

**État local au 2026-10-02 :** Visual Studio Community 2026 Insiders et le WDK/SDK 10.0.28000 sont présents. La compilation du pilote x64 Debug a réussi avec MSBuild amd64 ; `pnputil` a installé le catalogue après approbation du certificat de test généré. L'utilisateur a confirmé l'apparition et l'utilisation du moniteur virtuel. La sonde DXGI identifie la sortie virtuelle et la capture continue locale a été vérifiée avec le client ; le flux de production distant reste à sécuriser et à encoder.

### Phase 2 : preuve de capture

Laisser `IddSampleApp` actif dans le terminal administrateur lancé par `Test-Phase1.ps1`. Dans un second terminal, exécuter :

```powershell
.\Installer\Test-Phase2.ps1
```

Ce test n'exige pas l'administration. Il vérifie l'identifiant PnP `DELD0E6` du moniteur fourni par le sample, trouve la sortie DXGI correspondante et refuse de capturer si elle n'est pas unique. Il utilise DXGI Desktop Duplication uniquement sur cette sortie virtuelle, acquiert les mises à jour pendant 10 secondes, ne fait le readback CPU que pour une image de prévisualisation et affiche le nombre de frames ainsi que le FPS source moyen. Pendant le test, bouger le pointeur ou une fenêtre sur l'écran virtuel ; si rien ne change, peu de frames sont attendues. Cette mesure est celle des mises à jour de la source, pas encore le FPS rendu par le futur client réseau.

**Résultat vérifié le 2026-10-02 :** le moniteur IDD est identifié par PnP comme `DELD0E6`; son numéro `DISPLAYn` peut changer après reconnexion. Une frame BMP a été validée visuellement. Le relais continu ci-dessous capture la sortie IDD réelle.

### Prototype client et protocole de test

Le prototype est construit pour x64 avec MSBuild amd64 :

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe' .\Server\Network\DisplayStreamTestServer.vcxproj /m /p:Configuration=Debug /p:Platform=x64
& 'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\amd64\MSBuild.exe' .\Client\UI\DisplayClient.vcxproj /m /p:Configuration=Debug /p:Platform=x64
```

Pour tester sur une seule machine, lancer le serveur et le client dans deux PowerShell :

```powershell
& '.\Server\Network\x64\Debug\DisplayStreamTestServer.exe'
& '.\Client\UI\x64\Debug\DisplayClient.exe'
```

Le serveur envoie une mire BGRA animée de 160x90 à 10 FPS vers `127.0.0.1:48000`. Le client affiche la dernière frame complète et abandonne les fragments incomplets/obsolètes. `F` affiche/masque le HUD FPS, résolution, débit et pertes ; `F11` bascule le plein écran et `Échap` en sort. Le rendu GDI est uniquement un harnais basse résolution de phase 3/4, pas le renderer final pour le PC client.

Le générateur de mire et `DisplayCaptureStreamServer.exe` sont des outils de diagnostic UDP non chiffrés ; ils restent réservés à localhost. Ne passe pas `--allow-insecure-remote-test`.

### Capture réelle vers le client (boucle locale uniquement)

Garder `IddSampleApp` actif. Fermer le générateur de mire s'il tourne, puis lancer dans trois consoles :

```powershell
& '.\Client\UI\x64\Debug\DisplayClient.exe'
& '.\Server\Capture\x64\Debug\DisplayCaptureStreamServer.exe'
```

La deuxième commande cible `127.0.0.1` par défaut, repère le moniteur IDD par PnP, capture ses changements DXGI, les réduit par échantillonnage à 160x90 et envoie au maximum 10 FPS. Dans la fenêtre client, `F` affiche/masque les métriques, `F11` bascule le plein écran et `Échap` le quitte. `Ctrl+C` arrête le relais. La build Release du client, liée statiquement au runtime MSVC, se trouve dans `Client/UI/x64/Release/DisplayClient.exe`.

**Résultat vérifié :** le client a affiché le bureau virtuel réel via UDP loopback ; le HUD a montré environ 8 FPS, 160x90, environ 2,6 Mbit/s et 0 frame perdue pendant l'échantillon observé. Cela valide le chemin de démonstration, pas encore la latence ou la charge en jeu. La réduction actuelle fait un readback CPU du plein format avant rééchantillonnage ; elle est provisoire et ne convient pas au flux final 1080p/60.

**Sécurité :** le relais UDP de diagnostic reste loopback-only par défaut et son option distante non chiffrée ne doit pas être utilisée. Le relais distant sécurisé ci-dessous utilise TLS 1.2, vérifie le certificat épinglé et exige le PIN de session avant d'envoyer des frames. Le client UI lui-même n'écoute que sur localhost.

### Test sur le PC Windows 10

Le chemin de démonstration inter-PC utilise SChannel TLS 1.2, un pin SHA-256 du certificat serveur et un PIN aléatoire à usage de session. Le certificat public ne contient pas la clé privée et n'a pas à être importé dans les racines Windows. Le viewer ne reçoit que les frames après authentification ; le bridge client ouvre la connexion TLS sortante et ne transfère les images qu'à `127.0.0.1` pour le viewer.

Sur l'hôte Windows 11, construire `DisplayCaptureSecureServer.vcxproj` et `DisplayCaptureServerSetup.vcxproj` en Release x64, puis lancer `Server/Capture/x64/Release/DisplayCaptureServerSetup.exe`. La fenêtre montre le nom Windows et l'adresse IPv4 de l'hôte à saisir côté client.

La fenêtre indique les prérequis manquants. **Créer certificat** génère le certificat TLS nécessaire. Si l'écran IDD est absent, **Installer écran IDD** lance son installation avec une demande UAC ; cette étape requiert le WDK, l'approbation du certificat de test et de garder l'application du moniteur virtuel active. **Paquet client** construit les exécutables du client et prépare `Client/Package`. Cette préparation n'installe rien sur le PC client.

Copier le dossier `Client/Package` complet vers le PC client par un support ou canal de confiance. Il contient le lanceur graphique, le viewer, le bridge TLS et le certificat public ; ce dernier ne contient aucune clé privée.

Sur l'hôte, ouvrir uniquement le port TLS depuis l'adresse privée du client. Depuis un PowerShell administrateur, remplacer l'adresse par l'IPv4 du PC Windows 10 :

```powershell
New-NetFirewallRule -DisplayName 'MyDisplay TLS test' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 48001 -RemoteAddress '192.168.1.50' -Profile Private
```

Sur le PC Windows 10, extraire le dossier `Client/Package` complet et lancer `DisplayClientSetup.exe`. Le client est portable : aucune installation ni droit administrateur n'est requis. La fenêtre vérifie les fichiers, puis demande l'adresse IPv4 et le nom affichés par le serveur ainsi que le code à six chiffres. Le viewer et le bridge démarrent après validation. Utiliser `F` pour le HUD, `F11` pour le plein écran et `Échap` pour en sortir.

Avant le test vidéo, régler l'écran virtuel dans Paramètres > Système > Affichage sur **Étendre**, **1920x1080** et **60 Hz**. Le serveur encode en H.264 avec une cible CBR de **50 Mbit/s** et limite la capture à **60 FPS** ; le débit observé peut être inférieur sur une image statique. Le HUD du client (`F`) affiche les FPS et le débit réellement reçus. Le débit/FPS soutenu dépend aussi de l'encodeur Media Foundation et des performances des deux PC.

Dans la fenêtre serveur, vérifier l'adresse LAN précise (pas `127.0.0.1`), puis cliquer sur **Démarrer**. Le code de session s'affiche et peut être copié avec **Copier le code**. Le serveur n'accepte qu'une tentative par lancement ; si le code est rejeté ou la session terminée, cliquer sur **Démarrer** de nouveau pour générer un autre code. Après le test, arrêter le serveur avec **Arrêter** et supprimer la règle temporaire :

```powershell
Remove-NetFirewallRule -DisplayName 'MyDisplay TLS test'
```

**État du flux distant :** l'ancien transport BGRA brut a été remplacé par H.264 et un test synthétique local encode puis décode 60 images 1080p. Le chemin de capture réelle à 1080p/60 et son débit réseau soutenu restent à mesurer avec l'écran IDD actif ; le serveur fait encore un readback CPU de la surface DXGI avant conversion NV12.

## Contenu de la pré-alpha

### Paquet à remettre au PC client

Exécuter `Installer/Package-Client.ps1` sur l'hôte après avoir créé son certificat TLS. Le dossier `Client/Package` doit contenir exactement ces quatre fichiers, côte à côte :

- `DisplayClientSetup.exe` : assistant de connexion et vérification du paquet.
- `DisplayClient.exe` : fenêtre de visualisation.
- `DisplayClientTlsBridge.exe` : connexion TLS, vérification du certificat et relais local vers le viewer.
- `ServerCertificate.cer` : certificat public propre à cet hôte, sans clé privée.

Transférer le dossier complet par un canal de confiance et ne pas mélanger les certificats de plusieurs hôtes. Sur le client Windows 10 x64, extraire le dossier puis lancer `DisplayClientSetup.exe`. Aucun pilote ni droit administrateur n'est requis côté client. Les builds Release du client lient statiquement le runtime MSVC; les API Windows Media Foundation restent nécessaires. Ne pas ajouter les fichiers `.pdb`, `.ilk`, `.iobj` ou `.recipe` au paquet.

### PC hôte pour les essais

Les binaires Release sont `Server/Capture/x64/Release/DisplayCaptureServerSetup.exe` et `Server/Capture/x64/Release/DisplayCaptureSecureServer.exe`. Garder ces deux exécutables dans le même dossier. Le serveur utilise le certificat avec clé privée du magasin personnel de l'utilisateur Windows qui lance l'application; le fichier `.cer` public exporté par `New-ServerCertificate.ps1` est celui à remettre au client.

`DisplayCaptureServerSetup.exe` est encore un outil de développement, pas un installateur autonome : ses boutons lancent des scripts sous `Installer/`, et la création du paquet client et l'installation IDD compilent des projets. Pour utiliser tous ces boutons, conserver l'arborescence source `Client/`, `Common/`, `Installer/` et `Server/DisplayDriver/IddSample/` avec leurs sous-dossiers, installer Visual Studio C++ et le WDK compatibles, et garder les binaires serveur ci-dessus dans leur chemin Release d'origine. L'écran virtuel doit être installé et activé sur l'hôte avant de démarrer le flux.

### Pilote IDD de test

Pour un essai local seulement, `Installer/Test-Phase1.ps1` génère et installe le package depuis `Server/DisplayDriver/IddSample/`. Le package produit contient les fichiers référencés par son INF, notamment `IddSampleDriver.inf`, `IddSampleDriver.cat` et `IddSampleDriver.dll`, ainsi que le certificat de test généré et `IddSampleApp.exe`. Conserver ensemble le package complet généré par le WDK; ne pas reconstruire une distribution à partir des seuls `.exe` visibles dans `x64/`.

Le certificat du pilote de test et son approbation machine ne sont destinés qu'à une machine de test contrôlée. Ne pas remettre ce certificat comme certificat de confiance général et ne pas présenter ce package sample comme un pilote signé pour distribution. Pour une pré-alpha reproductible, remettre les sources du sample avec `Server/DisplayDriver/IddSample/LICENSE` et compiler/installer localement avec le WDK. Le PC hôte et le PC client doivent être sur un réseau privé; limiter la règle pare-feu TCP 48001 à l'adresse du client et la supprimer après l'essai.

### À exclure et limites connues

Ne pas inclure les dossiers `Debug/`, les fichiers intermédiaires Visual Studio, les clés privées TLS, ni les artefacts du test UDP non chiffré (`DisplayStreamTestServer.exe` et `DisplayCaptureStreamServer.exe`) dans un paquet destiné à être exécuté sur le réseau. Ce dernier reste réservé à localhost. Cette pré-alpha n'est pas un installateur grand public : elle n'a pas de signature de code publique, de mise à jour automatique, de découverte finalisée ni de réglages de résolution/débit. Valider d'abord l'image, la reconnexion, la charge CPU/GPU et le débit sur les deux machines ciblées.

## Références vérifiées

- [Indirect Display Driver Model Overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview)
- [Sample IddSample](https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/indirect-display-driver-sample/)
- [IddCxSwapChainReleaseAndAcquireBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/iddcx/nf-iddcx-iddcxswapchainreleaseandacquirebuffer)
- [IddCxSwapChainFinishedProcessingFrame](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/iddcx/nf-iddcx-iddcxswapchainfinishedprocessingframe)
- [IddCx API reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/iddcx/)
- [SwDeviceCreate](https://learn.microsoft.com/en-us/windows/win32/api/swdevice/nf-swdevice-swdevicecreate)
- [SwDeviceClose](https://learn.microsoft.com/en-us/windows/win32/api/swdevice/nf-swdevice-swdeviceclose)
- [Installer un certificat de test de pilote](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/installing-a-test-certificate-on-a-test-computer)
- [Télécharger le WDK et choisir les versions compatibles](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
- [DXGI Output Duplication](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutput1-duplicateoutput)"# MyDisplay" 
