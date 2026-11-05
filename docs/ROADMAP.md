# Feuille de route

La priorité produit est de conserver toutes les capacités Git pour le code tout en remplaçant le stockage des assets. Le mode autonome progresse sur le même socle, avec des critères de validation explicites. Les cases cochées décrivent le prototype présent dans ce dossier.

## 0 — Socle vérifiable

- [x] Bibliothèque de blocs indépendante, découpage CDC et fixe, BLAKE3.
- [x] Manifestes complets, compression, reconstruction et vérification d'intégrité.
- [x] Révisions autonomes complètes, branches, journal, export et détachement avec archive.
- [x] Pins, fermeture des dépendances, GC simulé puis explicite sous verrou exclusif.
- [x] Filtres Git clean/smudge et démonstration sur un vrai dépôt.
- [x] Serveur authentifié, agent ponctuel, comparaison des objets et reprise.
- [x] Chiffrement des objets au repos et interface C initiale.
- [x] ABI C de blocs, import vérifié, dépendances, pins et paramètres de découpage.
- [x] Migration C++20/CMake, compilation Windows x64 et tests locaux ; CI Windows/Linux/macOS.
- [x] Compatibilité croisée avec les stores Rust v1, dont le chiffrement, et fixtures indépendantes de Rust.
- [ ] Exécution réelle des matrices Linux/macOS et validation ARM64.
- [x] Arrêts forcés aux frontières d'écriture/publication, reprise avec le moteur de production et reconstruction des versions conservées, en clair/chiffré ; campagne locale Windows x64.
- [ ] Validation de corpus Unreal, exécution multiplateforme de la campagne de crash, injections d'erreurs système et coupures d'alimentation.

Critère de sortie : aucune perte des versions conservées après suppression de racines, reprise de transfert ou panne contrôlée ; reconstruction identique octet par octet sur les plateformes cibles.

## 1 — Usage Git quotidien

- [x] Catalogue déterministe des pointeurs d'un commit Git et racine locale vérifiée conservant ses assets.
- [x] Push/pull groupés des catalogues, racines distantes immuables, validation en staging et publication par catalogue.
- [x] Hook pre-push opt-in : parcours de toute l'ascendance, blocs des versions intermédiaires, baux et refus de publication si les assets manquent.
- [ ] Validation côté serveur Git et coordination distribuée de la publication ; le hook local reste contournable.
- [x] Téléchargement à la demande au checkout, baux de lecture, contrôle d'intégrité avant sortie et clones à pointeurs seuls (`local`/`download`/`skip`).
- [x] Protocole `filter-process` v2 séquentiel, Store/cache conservés entre fichiers d'une commande Git, messages bornés et reprise après erreur d'un fichier.
- [ ] Hydratation différée (`delay`) et cache partagé entre processus/worktrees.
- [x] Migration hors ligne d'un snapshot Git LFS vers un commit enfant sur une nouvelle branche : audit SHA-256, import dédupliqué, attributs et rétention complète avant publication, sans modifier HEAD/index/workspace.
- [ ] Réécriture de tout l'historique LFS, téléchargement depuis les serveurs LFS et prise en charge des extensions de pointeurs.
- [ ] Compatibilité explicite GitHub/GitLab et documentation d'hébergement des blocs.
- [x] Inventaire des pointeurs par branches, tags, index, reflogs, stashes et worktrees ; aperçu vérifié, ajout atomique des protections manquantes et rapport des pins non observés.
- [ ] Retrait automatique coordonné des anciennes protections Git, politique de récupération et inventaire incrémental ; prise en compte des fichiers de travail et pseudo-références opérationnelles.
- [ ] Exécutable installable, configuration par dépôt, mises à jour et désinstallation.

Critère de sortie : un clone neuf peut récupérer un projet et un push ne peut pas publier de pointeur dont les blocs manquent sur le service associé. Une erreur réseau reste récupérable sans perte de données.

## 2 — Performances de production

- [ ] Corpus : assets Unreal réels, sources, textures, audio, données compressées et fichiers dépassant la RAM.
- [x] Pipeline d'ingestion avec compression/chiffrement parallèles, plafond d'octets et de tâches ; écriture coordonnée et annulation coopérative C++.
- [x] Réglages persistants par dépôt, surcharges explicites et propagation du pipeline aux commits natifs et filtres Git.
- [x] Progression C++ et CLI (texte anglais/français ou JSON sur stderr), annulation coopérative des imports, commits, reconstructions et filtres ; protection des références et destinations avant publication.
- [x] Audits `fsck` interruptibles, progression des vérifications et restauration des sessions/caches après annulation.
- [x] Exports natifs préparés puis publiés par renommage exclusif ; annulation, collisions et arrêts forcés vérifiés.
- [x] Progression et annulation de l'agent HTTP, reprise des objets vérifiés et résultat explicite si la réponse de publication distante est perdue.
- [x] GC et empaquetage interruptibles pendant la préparation ; nettoyage terminé après publication, avec tests d'annulation aux frontières exactes.
- [ ] Validation des signaux et du renommage exclusif sur les autres plateformes.
- [x] Cache borné de lectures vérifiées, contrôle de l'enveloppe à chaque accès et validation des fichiers dans la traversée des dépendances.
- [ ] Index transactionnel des objets vérifiés ; validation incrémentale des dépendances.
- [x] Pack immuable indexé, activation physique v2 explicite, compaction GC et reprise aux frontières de publication simulées.
- [x] Handle de lecture de pack partagé pendant une opération, fermé avant publication/suppression et après les exceptions.
- [x] Catalogue v3 de packs multiples, ajout incrémental, limites de taille/objets, regroupement explicite et GC sélectif par pack.
- [x] Sessions de lecture avec au plus 8 handles de packs ouverts et éviction LRU.
- [x] Index de packs consultés par pages vérifiées, répertoire/filtre compacts, cache LRU borné et écriture en flux sans changer le format v2/v3.
- [x] GC avec inventaires en flux, bitmap des objets empaquetés et réécriture/relecture des survivants sans liste complète d'IDs.
- [x] Audit `fsck` avec inventaire en flux, énumération par type et bitmap ; validation des objets non référencés et des deux copies physiques.
- [x] Empaquetage/compaction avec fusion paginée des index, plan borné par pack, source vérifiée sur deux passes et relecture sans liste complète d'IDs empaquetés.
- [ ] Fusion automatique des petits packs et validation de charge du moteur complet ; borner le tri et les marques des clés isolées, la pile de parcours et les autres métadonnées.
- [x] Lecture de plages exactes à partir des manifestes v1, vérification des seuls blocs concernés et export exclusif ; API C++/C/Go et CLI, en clair/chiffré et sur les trois stockages physiques.
- [x] Plans de réutilisation entre manifestes, blocs déplacés/répétés, déduplication des besoins et compteurs logiques ; comparaison de métadonnées C++/C/Go et CLI sans lire les payloads.
- [x] Tables de références sans parcours quadratique de la table, sérialisation triée locale/HTTP et garde de profondeur JSON en SAX ; validation de 32 768 pins en clair/chiffré et mesure CLI reproductible.
- [ ] Arbres Merkle paginés, manifestes hiérarchiques et index de plages persistant.
- [ ] Index de travail, watcher et journal de changements pour éviter les rescans.
- [x] Requêtes HTTP d'objets parallèles, plafonds de requêtes et de buffers, imports coordonnés et déduplication des requêtes en vol.
- [x] Compression réseau Zstandard négociée, repli brut, décodage borné, budgets de préparation et compteurs logiques/réseau distincts.
- [ ] Cache des présences et validation de charge de la compression sur des corpus réels.
- [x] Retry/backoff bornés des requêtes idempotentes, attente annulable et cibles de débit partagées entre les requêtes ; publication finale jamais relancée automatiquement.
- [x] Journal local persistant, reprise explicite/automatique, références attendues conservées et protection des objets transférés contre le GC local ; réconciliation des publications incertaines sans rejouer le CAS.
- [x] Baux d'envoi serveur persistants avec expiration, renouvellement, fencing, reçus de publication et administration hors ligne ; refus du GC tant qu'un bail est actif.
- [x] Baux de lecture persistants pour les pulls : racine exacte, reprise après suppression de référence, renouvellement asynchrone et libération avant publication locale.
- [x] Rétention sélective des sources locales, pulls incomplets et baux de lecture ; collecte des objets sans lien avec les transferts, avec validation des fichiers complets.
- [x] Purge locale des reçus terminés, aperçu par défaut, validation complète avant suppression et conservation des sessions inachevées.
- [x] Rétention sélective des envois serveur avant réception du manifeste : inventaires paginés, engagement des pages avant négociation, intégrité, reprise et purge des générations obsolètes.
- [ ] GC distribué avec protection coordonnée entre serveurs et réplication.
- [ ] Stockage distant compatible S3, caches locaux et réplication vérifiée.

Critère de sortie : budgets CPU/RAM/IO documentés, débits p50/p95 et octets transférés mesurés ; aucun réglage de vitesse ne dégrade les garanties de reconstruction. Les cibles chiffrées doivent être fixées à partir des machines et des corpus de référence.

## 3 — Équipes et Unreal Engine

- [ ] Verrous d'assets exclusifs avec propriétaire, lease, expiration et fencing token.
- [ ] Validation serveur des verrous à la publication ; gestion des branches et mode hors ligne.
- [ ] Rôles et ACL par dépôt/chemin, audit et identité des appareils/utilisateurs.
- [ ] Plugin Unreal : état, historique, locks, submit, revert et résolution des conflits.
- [ ] Workspaces partiels, sélection de Content/Plugins et téléchargement à la demande.
- [ ] Prise en charge des ensembles `.uasset/.uexp/.ubulk`, World Partition et external actors.
- [ ] Diff visuel d'assets et intégration aux outils de merge Unreal quand le type le permet.
- [ ] Renommages cohérents, respect de la casse, fichiers en cours de sauvegarde et limites de chemins.

Critère de sortie : deux artistes ne peuvent pas publier silencieusement des modifications incompatibles d'un asset verrouillé ; un workspace partiel conserve une vue cohérente de ses dépendances.

## 4 — Synchronisation continue et Syncthing

- [ ] Agent résident avec watcher, rescans de récupération, exclusions et journal persistant.
- [ ] Modèle causal des versions, tombstones, détection de modifications concurrentes et copies de conflit.
- [ ] Publication cohérente de plusieurs fichiers, suppressions et renommages.
- [ ] Planificateur multi-source, découverte et identité des appareils, gestion hors ligne.
- [x] ABI C étendue et binding Go ; banc de copie différentielle, reprise et reconstruction sous Windows x64.
- [ ] Exécution réelle du backend cgo Linux/macOS, validation ARM64 et mesure des coûts d'interface.
- [ ] Adaptateur BEP à blocs fixes, ou extension CDC négociée entre pairs adaptés.
- [ ] Tests d'interopérabilité avec Syncthing ; mesurer le coût du passage Go/C++ via l'interface C.

Critère de sortie : après partitions réseau et modifications concurrentes, convergence sans écrasement silencieux. La compatibilité Syncthing ne sera annoncée qu'après des tests contre sa propre implémentation.

## 5 — Contrôle de versions autonome complet

- [x] État du dossier natif par hash complet, exclusions partagées avec le commit et comparaison des snapshots par contenu, mode et représentation ; API C++ et CLI interruptibles sans écriture.
- [x] Staging persistant par branche, sélection de fichiers/suppressions, inspection et retrait de sélections sans toucher au workspace ; snapshots protégés contre le GC, base attendue et commit/consommation atomiques.
- [x] État combiné base/index/workspace, conservation de l'information d'index périmé et distinction des deux côtés d'un même chemin.
- [x] Plan de checkout en lecture seule : actions, vérification complète optionnelle, fichiers suivis propres par hash, collisions non suivies/ignorées, liens/jonctions, casse et stockage protégé ; API C++ et CLI interruptibles.
- [x] Premier checkout récupérable sur workspace suivi propre : préparation vérifiée, déplacements exclusifs, journal persistant, reprise/annulation, publication atomique de branche, archive source et rétention cible ; tests d'arrêts forcés.
- [x] HEAD symbolique local et branche courante implicite main pour les anciens stores ; changement de branche récupérable sans déplacer les deux têtes, publication atomique du HEAD avec journal, commandes CLI par défaut sur la sélection courante.
- [x] Tags natifs simples/annotés, noms immuables par l'API, inspection paginée bornée, suppression archivée avec cible attendue et restauration atomiques ; rétention après GC et sélecteurs de révision HEAD/branche/tag avec ambiguïtés explicites.
- [ ] Sélection de hunks, diff textuel et rebase automatique d'un index périmé.
- [ ] HEAD détaché, workspaces multiples et checkout avec transitions fichier/dossier et renommages de casse ; nettoyage explicite des reçus/archives de checkout, tags et amendements.
- [ ] Merge à trois voies, base commune, conflits de texte, conflits binaires et renommages.
- [x] Parcours itératif borné du graphe de révisions, relations d'ascendance et tous les meilleurs ancêtres communs, y compris les merges croisés ; lecture seule sans lire les blocs de fichiers.
- [x] Plans de cherry-pick/revert à trois voies par fichier : contenu/mode et représentations distincts, conflits de contenu/ajouts/suppressions et collisions du résultat, parent principal explicite pour les merges, contrôle du workspace et vérification optionnelle sans écriture.
- [x] Amendement du dernier commit natif par dossier, index préparé ou message seul ; parents conservés, tête attendue et publication atomique branche/archive/index, reconstruction après GC et arrêts forcés.
- [x] Cherry-pick et revert par fichier avec commit enfant et application récupérable : tête attendue, données vérifiées, conflits bloquants, branche source intacte, reprise/annulation et conservation après GC.
- [ ] Rebase, reset, stash et squash ; résolution interactive des conflits de rejeu et fusion textuelle.
- [ ] Blame, bisect, suivi des renommages et recherche dans l'historique.
- [ ] Auteurs, dates, signatures et politiques de validation des commits.
- [ ] Push/pull des références avec validation d'ascendance et politiques serveur.
- [ ] Import/export Git préservant l'information représentable dans les deux modèles.

Critère de sortie : scénarios de travail complets comparés à Git, avec historique et résolution des conflits vérifiables. La représentation actuelle des révisions ne suffit pas à annoncer ces fonctions comme disponibles.

## 6 — Sécurité, rétention et exploitation

- [ ] Chiffrement de bout en bout, manifestes opaques et racines authentifiées.
- [ ] Dérivation de clés, coffre système, rotation, révocation et récupération.
- [ ] Modèle explicite de fuite d'égalité ; déduplication bornée au domaine autorisé.
- [ ] Rétention par durée/release, archives protégées, leases et GC distribué en deux phases.
- [ ] Distinguer cache local évictable et copie durable : ne jamais évincer l'unique exemplaire.
- [ ] Backups restaurables, réplication, scrubbing périodique et réparation des blocs manquants.
- [ ] Quotas, limitation de charge, métriques et journaux sans secrets.
- [x] Refus HTTP 401 après réception du corps borné, avant accès au stockage ; régression Windows avec corps différés, framing chunked, authentification dupliquée et requêtes concurrentes.
- [ ] Fuzzing des formats/protocoles, injections de pannes et audit de sécurité.
- [ ] Choix de licence, provenance des dépendances, paquets signés et stratégie de migration de format.

Critère de sortie : restauration complète après perte d'un nœud et après rotation de clés ; aucune suppression ne précède la validation de toutes les racines protégées et des transferts en cours.
