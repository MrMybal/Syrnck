---
id: native-authorship
title: Auteurs natifs et signatures
description: Auteur d’origine, committer actuel et signatures Ed25519 facultatives.
section: Versionnage
order: 305
parent: native-history
sourceRevision: 5
---

## Configurer une identité {#identity}

```text
syrnck native identity --name "Alice" --email "alice@example.invalid" --timezone-offset +60
syrnck native identity
syrnck native log --json
syrnck native log --show-signature
syrnck native identity --clear
```

Nom et adresse sont des textes UTF-8, limités à 256 et 320 octets, sans caractères
de contrôle. Le fuseau compte les minutes à l’est d’UTC dans -1439..1439, avec 0
par défaut. L’adresse est enregistrée littéralement sans prouver sa propriété.
Champs JSON, IDs et empreintes de clés sont identiques dans les interfaces
anglaise et française.

Le fichier facultatif `native-identity.json` du Store, propre à cette machine,
contient uniquement version, type, nom, adresse et fuseau. Il s’applique à tous
les workspaces utilisant ce Store. Les sauvegardes logiques ne le copient pas.
Effacer le profil préserve révisions existantes et exigence de lecteur additive.

La paire `SYRNCK_USER_NAME` et `SYRNCK_USER_EMAIL` remplace le profil pour une
invocation ; `SYRNCK_USER_TIMEZONE` fournit un décalage facultatif. Sans identité
configurée, une nouvelle révision reste en v1, y compris après effacement
explicite du profil. Réécrire une révision v2 exige une identité actuelle.

## Signer sans enregistrer la clé privée {#signing}

La variable facultative `SYRNCK_SIGNING_SEED` fournit une graine Ed25519 de
32 octets sous forme de 64 caractères hexadécimaux. Utiliser un environnement
de processus contrôlé ; ne pas inscrire une vraie graine dans les arguments,
scripts ou fichiers de profil. Le CLI consomme puis retire la variable de son
environnement et efface ses propres tampons temporaires. Les autres commandes
la retirent avant de lancer des outils. Cela ne promet pas d’effacer les copies
antérieures du système ou de l’appelant. Sans graine, une identité crée une
révision v2 non signée, avec `signature: null`.

Dans la bibliothèque C++, identité et clé sont explicitement liées au handle :

```cpp
#include <syrnck/core.hpp>
store.use_native_identity({"Alice", "alice@example.invalid", 60},
                          syrnck::SigningKey::generate());
auto revision = store.commit_directory(project, "main", "Add asset");
store.clear_native_identity();
```

Le Store possède exclusivement la clé déplacée. La bibliothèque ne lit pas
automatiquement profil CLI ou environnement. `native_identity()` expose profil
et clé publics ; aucun export de clé privée.
`take_native_identity()` détache un `NativeIdentityContext` déplaçable contenant
profil et éventuelle clé. Déplacer les deux vers le Store suivant avec
`use_native_identity()` avant de libérer le contexte. Un processus résident peut
ainsi utiliser des handles verrouillés courts sans copier ni exporter la graine.
L’ancien handle devient non configuré ; prendre un contexte vide renvoie null.
Changer ou détacher l’identité dans un callback de progression est refusé. Voir la
[primitive de signature](doc:signing-api) pour propriété des graines et vérification.

## Règles d’auteur et de committer {#rewrites}

Une révision v2 enregistre `author`, `committer` et une signature facultative.
Chaque identité datée contient `name,email,timestamp,timezone_offset` ; la date
du committer égale celle de la révision. Un auteur d’origine peut être null
lors d’une réécriture d’histoire ancienne. Le committer actuel signe le résultat
complet, fichiers et parents compris. Une ancienne signature n’est jamais copiée
sur des métadonnées modifiées.

Nouveaux commits de dossier/index, nouveaux merges, reverts et squashes autonomes
utilisent la personne actuelle comme auteur et committer. Amend, cherry-pick,
rebase linéaire et détachement d’histoire préservent l’auteur d’origine avec le
nouveau committer. Squash/fixup du rebase gardent l’auteur du premier commit
retenu. Résultat inchangé ou fast-forward : révision existante exacte conservée.
Les snapshots internes d’index, stash et récupération restent v1 sans auteur inventé.

Le rebase fige une date de committer commune aux deux calculs de candidats.
Avec une identité, `--committer-time SECONDS` sur rebase-plan/rebase-todo/rebase
rend l’aperçu reproductible avec les mêmes identité, clé et entrées. Sans cette
option, des invocations distinctes peuvent choisir des dates et IDs différents.
Le rapport expose `committer_timestamp`. Une récupération checkout utilise les
candidats signés figés sans avoir besoin de la clé privée de signature.

`native log --json`, historique de fichier, blame et résumés d’archives exposent
auteur, committer et ID public de clé pour v2. Le log texte affiche l’auteur et
le committer différent ; `--show-signature` ajoute l’ID ou l’état non signé.
Champs des résumés anciens et parcours par premier parent restent compatibles.

## Octets signés et compatibilité {#format}

La révision v2 est un JSON compact canonique ordonné
`version,parents,message,timestamp,author,committer,signature,files`.
La signature contient exactement `algorithm,key_id,public_key,value` :
`ed25519`, empreinte, clé publique de 32 octets et signature de 64 octets ; les
valeurs binaires utilisent l’hexadécimal minuscule. Le message signé est l’ASCII
`Syrnck native revision signature v1:` suivi des 64 caractères hexadécimaux
minuscules du BLAKE3 de la révision v2 canonique avec `signature` à null.

Import, lecture et audit valident signature, empreinte et révision canonique.
Modifier auteur, parents, date, message ou fichiers invalide une signature.
`authored-revisions-v1` est activé avant stockage d’un objet v2 ou d’un profil.
Les anciens lecteurs refusent cette configuration étendue ; enveloppe, manifest,
pack, objets HTTP et ABI C restent inchangés. Sauvegardes/restaurations logiques
préservent l’exigence. Révisions v1 et v2 peuvent coexister dans le même graphe.

## Confiance et limites restantes {#trust}

Une signature valide prouve la possession de sa clé pour ces octets. Nom,
adresse et date restent des déclarations du signataire. L’accès serveur v1
utilise principal authentifié, ACL de branches/chemins et verrous d’assets ;
[l’accès v2](doc:server-signatures) peut aussi imposer des signatures approuvées
et lier clés/identités de committer aux comptes. Il vérifie la politique actuelle
pour l’histoire nouvellement admise tout en préservant la base déjà publiée.
Coffre système de signature et révocation distribuée restent à développer. Les [API d’auteur C/Go](doc:go-binding#native-authorship) configurent des identités
locales au handle et préparent des révisions signées neuves. Une signature exigée
peut bloquer les nouvelles révisions sans graine tout en gardant lecture/reprise
publiques. `NativeIdentityContext` C++ conserve cette politique avec le signataire
exclusif transféré entre handles. Politique et clé privée ne sont pas persistées.

Les graines de signature protégées CLI/C++/C/Go sont disponibles dans le [guide des signataires protégés](doc:signing-keys). Un échec d’authentification du fournisseur fichier ne produit pas de capture non signée ; la politique obligatoire reste locale au processus sauf règle serveur.
