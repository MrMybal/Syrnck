# Packs et compaction

`syrnck pack` regroupe les objets isolés dans des packs immuables indexés. Il conserve les identifiants, enveloppes `SYR1`, nonces de chiffrement, manifestes et références. La maintenance ordinaire ajoute des packs sans réécrire ceux déjà actifs ; `pack --compact` regroupe explicitement tout le stock. Aucune de ces commandes ne supprime de version logique. L'API C++ correspondante est `Store::pack(PackOptions)`.

```text
syrnck --store donnees pack
syrnck --store donnees pack --compact
syrnck --store donnees pack --max-mib 512 --max-objects 65536
syrnck --store donnees gc
syrnck --store donnees gc --delete
```

## Activation et compatibilité

Un store neuf reste au format physique v1, avec un fichier par objet. La première commande `pack` publie une configuration **v2** si un seul pack suffit, ou **v3** si plusieurs packs sont nécessaires. Ajouter un deuxième pack à un store v2 passe à v3. Les objets logiques et le protocole réseau restent v1. Le moteur lit les trois configurations ; les binaires plus anciens refusent les versions physiques qu'ils ne comprennent pas.

Le format v3 contient une liste de packs actifs. Une fois activé, il est conservé même si une compaction ou un GC réduit cette liste à un seul pack ou à zéro. Les stores existants ne sont pas convertis automatiquement à l'ouverture. Il n'existe pas de commande de retour à une ancienne version physique.

Les nouvelles écritures continuent à produire des fichiers isolés. La lecture combine ces objets et tous les packs actifs. Une clé peut avoir une copie isolée et une copie dans un pack ; elle compte une fois dans l'inventaire logique. La copie isolée est prioritaire pour les lectures ordinaires. `fsck` contrôle aussi la copie du pack pour qu'un doublon valide ne masque pas une corruption. Une même clé dans deux packs actifs est refusée comme catalogue incohérent.

Le CLI, les filtres Git, le serveur, l'ABI C et le binding Go lisent ce stockage par le même moteur. Le réseau échange toujours des objets logiques vérifiés ; les pairs peuvent employer des dispositions physiques et des clés différentes. La maintenance est exposée par le CLI et l'API C++, sans nouvelle fonction C/Go.

## Regroupement incrémental et complet

Par défaut, `pack` sélectionne les objets isolés qui ne figurent dans aucun pack actif. Il les répartit entre de nouveaux packs, puis ajoute ces packs au catalogue. Les fichiers existants gardent leurs noms, octets et dates d'écriture. Les copies isolées redondantes sont retirées après validation. Dans un store déjà au format v2/v3, sans objet nouveau ni copie à nettoyer, la commande ne réécrit ni pack, ni index, ni configuration ; des restes de générations non publiées peuvent toutefois être nettoyés.

`pack --compact` rassemble les objets de tous les packs et les objets isolés dans un nouvel ensemble de packs. Cette opération permet de réduire leur nombre. Elle respecte les mêmes limites de taille et de nombre d'objets ; elle ne garantit donc pas de produire un pack unique.

`--max-mib` accepte **18 à 4 096 Mio**, avec **512 Mio** par défaut. `--max-objects` accepte **1 à 1 048 576**, avec **65 536** par défaut. Ces plafonds s'appliquent aux packs créés par la commande, en-tête de 8 octets inclus pour le plafond d'octets. Les enveloppes sont conservées entières ; une enveloppe maximale de 17 Mio doit pouvoir tenir. Les réglages C++ sont `PackOptions::max_pack_bytes`, `max_objects_per_pack` et `compact`. Le minimum C++ exact est 17 Mio + 8 octets.

Le plan est calculé à partir des tailles stockées, puis les limites sont à nouveau appliquées aux enveloppes effectivement lues. Un plan dépassant le nombre disponible de packs est refusé avant la construction. Il faut alors augmenter les limites de création ou utiliser `--compact` pour regrouper les packs existants.

Le résultat JSON distingue `objects`, `pack_count`, `pack_bytes` et `index_bytes` du catalogue final, de `new_packs`, `objects_written`, `pack_bytes_written` et `index_bytes_written` pour les nouvelles publications physiques. Les compteurs d'octets portent sur les fichiers complets pack/index. `loose_objects_removed` et `obsolete_pack_files_removed` décrivent les retraits physiques. La validation complète lit toujours le stock existant : le regroupement incrémental réduit les réécritures, sans rendre cette validation constante en temps.

## Publication et reprise

Sous le verrou exclusif du store, `pack` :

1. Recharge la configuration et les index, vérifie les objets, leurs dépendances et les hashes complets des fichiers, y compris les objets non épinglés.
2. Prépare le plan, écrit les nouveaux packs en flux, puis leurs index, avec les publications atomiques et flushs du moteur.
3. Relit les nouvelles enveloppes et revalide leurs identifiants, formats et authentification.
4. Remplace atomiquement `config.json`, qui publie le catalogue complet.
5. Retire les copies isolées et les générations devenues inactives.

Une erreur avant le remplacement de la configuration conserve l'ancien catalogue et ses copies. Un lot peut laisser des packs non publiés si la construction d'un pack suivant échoue. Après publication, une interruption peut laisser des doublons et d'anciens packs ; le catalogue désigne déjà les nouvelles copies complètes. `pack` ou `gc --delete` nettoient ces restes après validation. Seuls les noms générés `pack-<32 caractères hexadécimaux>.syp/.syi` inactifs sont concernés ; les liens symboliques à ces emplacements sont refusés.

Un échec de flush après le remplacement atomique peut survenir alors que le nouveau catalogue est déjà visible. Le handle recharge alors la configuration effective avant de rendre l'erreur ; un échec de rechargement invalide l'index en mémoire. Il faut corriger le problème et rouvrir le store ou réussir un nouvel audit avant de poursuivre les accès aux objets.

Les tests simulent les états avant et après publication, ainsi qu'une exception pendant la construction du deuxième pack d'un lot. Ils ne constituent pas une campagne de coupures d'alimentation ou d'injections d'erreurs système à chaque appel IO. La suite locale Windows x64 couvre corruption, limites, réouverture, chiffrement, conservation des packs inchangés, archives et transferts. Le test optionnel de liens symboliques reste ignoré localement faute du privilège Windows nécessaire ; les matrices Linux/macOS restent à exécuter. L'ancien moteur Rust a aussi refusé les catalogues v3 en clair et chiffrés sans modifier leurs fichiers.

## GC sélectif

Le GC commence par valider la fermeture de toutes les références et les hashes des fichiers conservés, sans utiliser les données du cache. Une racine absente ou corrompue arrête l'opération avant toute suppression. Supprimer une référence ne supprime pas un bloc encore utilisé par une autre branche, archive, révision ou pin.

Chaque pack est traité séparément :

- Tous ses objets sont conservés : ses fichiers restent inchangés.
- Aucun n'est conservé : il est retiré du catalogue, sans recopier son contenu.
- Une partie est conservée : un nouveau pack contient ses seuls objets vivants, avec relecture vérifiée avant publication.

Toutes les modifications de catalogue du GC sont publiées ensemble avant le retrait des anciens fichiers. Les objets isolés conservés restent isolés. Le GC ne fusionne pas les packs ; utiliser `pack --compact` pour cela. Le pack de remplacement ne dépasse pas le nombre d'objets ni la taille du pack source, même si celui-ci provient d'un ancien store avec des packs plus grands que les réglages actuels par défaut.

Le GC simulé ne crée ni pack ni configuration. `reclaimable_bytes` additionne les enveloppes inaccessibles de toutes leurs copies physiques, hors en-têtes/index. `repack_required` indique si le catalogue contient des objets à retirer. `packs_to_rewrite`, `packs_to_drop` et `pack_bytes_to_write` annoncent les packs à recopier, les packs entièrement morts et les octets des seuls nouveaux packs de remplacement, en-têtes inclus mais index exclus. `obsolete_pack_files` et `obsolete_pack_bytes` décrivent séparément les restes déjà inactifs au début de l'opération.

## Mémoire du GC

Le GC parcourt les index par pages de 256 entrées. Le marquage des objets empaquetés utilise `somme(ceil(objets_du_pack / 8))` octets, soit **128 Kio pour 1 048 576 objets dans un pack**, hors overhead des conteneurs. Il conserve seulement un compteur de survivants et une taille par pack. Les objets morts et les survivants à recopier ne sont plus rassemblés dans des listes d'identifiants. Le constructeur de pack en flux vérifie le nombre annoncé et l'ordre strict des clés, puis la relecture contrôle toutes les nouvelles enveloppes avant publication. Les formats physiques restent identiques.

Les objets vivants absents du catalogue de packs utilisent encore un ensemble de clés binaires typées de 33 octets, avec l'overhead d'un arbre ordonné. Un store uniquement en fichiers isolés conserve donc un coût de marquage proportionnel à ses objets vivants. La pile de parcours, les références, les métadonnées JSON, les codecs, les répertoires/filtres des index et leurs caches s'ajoutent au bitmap. Le GC n'annonce pas un plafond global de RAM.

L'inventaire des objets isolés est parcouru une première fois pour valider toute son arborescence et préparer les compteurs, avant toute écriture de remplacement ou suppression. Après publication éventuelle, un second parcours retire les objets isolés morts. Les copies à la fois isolées et empaquetées comptent une fois dans `live_objects`/`unreachable_objects`, et chaque copie morte contribue aux octets récupérables. Le nettoyage utilise le nouveau catalogue après publication : les positions du bitmap de l'ancien catalogue ne sont jamais réinterprétées dans le nouveau.

Le résultat JSON ajoute trois compteurs décrivant le travail de l'opération : `packed_mark_bytes` pour les octets utiles du bitmap initial, `loose_live_objects` pour les clés vivantes absentes du catalogue initial et `peak_pending_objects` pour le maximum de clés en attente dans la pile. Ils ne mesurent ni le RSS ni toutes les allocations. Les caches vérifiés restent exclus de la validation des racines. Le verrou exclusif couvre les deux parcours et la publication.

Les tests spécifiques comparent le résultat à un oracle utilisant des ensembles complets, avec des centaines d'objets, plusieurs pages/packs, des doublons physiques, des blocs répétés, un cache d'index nul et des stores clairs/chiffrés. Ils vérifient aussi les échecs de source/compte/ordre du constructeur en flux et l'absence de publication ou suppression si l'inventaire isolé est invalide. Les suites existantes conservent les scénarios d'archives, de corruption et de reprise.

## Limites actuelles

Le catalogue accepte **1 024 packs**, **1 048 576 objets par pack** et **4 194 304 objets empaquetés au total**. La taille binaire totale des index reste `16 × packs + 45 × objets` octets. Le lecteur les parcourt à l'ouverture puis consulte des pages de 256 entrées, avec un répertoire, un filtre et une LRU de **4 Mio** par défaut (`--index-cache-mib`). La validation des doublons entre packs utilise des parcours fusionnés. Voir les [garanties de snapshot et budgets mémoire](INDEXES.md). Ces plafonds ne constituent pas une validation de charge du moteur complet à plusieurs millions d'objets.

La construction lit les objets en flux, sans charger les packs entiers en RAM. La compaction nécessite l'espace des nouveaux packs avant de retirer les anciens. Elle conserve les enveloppes, sans les recompresser ni changer la clé. Une reconstruction, une fermeture de dépendances ou un audit réutilise au plus **8 handles de packs** dans sa session, avec éviction LRU ; les appels imbriqués partagent cette session. Tous ces handles sont fermés au retour, y compris sur exception, et avant publication/suppression de générations. Les lectures isolées ouvrent leur propre handle. Chaque enveloppe reste relue et contrôlée avant tout hit du cache.

Les arbres de métadonnées paginés, le choix automatique des packs à fusionner et la maintenance en ligne avec lecteurs concurrents restent à développer. `objects()`, l'API publique de fermeture des dépendances et les plans de `pack`/compaction matérialisent encore des inventaires proportionnels au nombre d'objets. `fsck` parcourt désormais l'inventaire en flux avec un bitmap pour les objets empaquetés ; voir les [garanties et limites des audits](AUDIT.md). Voir aussi le [format binaire](PROTOCOL.md), les [mesures](PERFORMANCE.md) et la [feuille de route](ROADMAP.md).
