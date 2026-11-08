---
id: signing-api
title: API de signature Ed25519
description: Clés en mémoire à déplacement exclusif, signatures standard et vérification stricte.
section: API
order: 650
parent: integrations
sourceRevision: 1
---

## Signer et vérifier des octets {#usage}

```cpp
#include <syrnck/signing.hpp>
auto signer = syrnck::SigningKey::generate();
auto public_key = signer.public_key();
auto signature = signer.sign(payload);
bool valid = syrnck::verify_ed25519(public_key, signature, payload);
auto fingerprint = syrnck::signing_key_id(public_key);
```

`payload` est une vue d’octets ; les messages vides sont acceptés. Clés publiques
de 32 octets, signatures de 64 octets. Il s’agit d’Ed25519 avec SHA-512, défini
par la [RFC 8032](https://www.rfc-editor.org/rfc/rfc8032), fourni par le module
optionnel de Monocypher 4.0.3 embarqué. La variante EdDSA avec BLAKE2b de base
est un autre algorithme. Cette API n’exige ni OpenSSL ni exécution réseau.

## Propriété des clés {#ownership}

`SigningKey::from_seed(bytes)` accepte exactement 32 octets, copie l’entrée et
développe sa clé privée en interne. L’appelant reste propriétaire de son entrée.
`generate()` utilise l’aléa du système. L’objet se déplace sans pouvoir être
copié, n’expose aucun export secret et efface son secret développé à destruction.
Un objet déplacé refuse signature et accès à sa clé publique. `public_key()`
renvoie une copie publique. Utiliser des graines de signature indépendantes des
clés de chiffrement.

L’empreinte commence par `ed25519:`, suivi du BLAKE3 de la clé publique précédée
du domaine `Syrnck Ed25519 public key v1:`. Cet identifiant ne prouve pas qu’une
personne possède la clé. L’appelant doit autoriser la clé publique séparément
avant d’attribuer un contenu signé à une identité.

## Contrat de vérification {#verification}

La vérification renvoie faux pour longueurs incorrectes, messages ou signatures
altérés, encodages de points non canoniques et clés publiques de petit ordre.
Ces dernières ne permettent pas d’authentifier un signataire. La primitive amont
d’équation de courbe vérifie ce cas ; le moteur n’implémente pas son propre
calcul elliptique. L’encodage refuse aussi les éléments hors plage canonique et
les zéros négatifs. L’amont vérifie scalaire de signature et équation de courbe.

L’appelant définit les octets signés et leur domaine. Cette API n’établit pas
confiance d’identité, horodatage fiable, unicité de signature ou validité de
certificat. Auteurs natifs, signature automatique des commits, autorisation des
clés côté serveur et coffre de clés système restent des travaux distincts.
Les formats de révision existants restent inchangés.

## Construction et vérification {#validation}

```text
python scripts/build.py build --target syrnck_signing_tests
python scripts/build.py test -R "^signing$"
```

Les tests couvrent trois vecteurs RFC 8032, chaque modification d’un bit de leurs
signatures, messages modifiés, longueurs incorrectes, clés faibles, encodages non
canoniques et déplacements de clé. Un test construit une signature avec zéro
négatif satisfaisant l’équation amont, puis exige son refus strict par le moteur.
Ces contrôles fonctionnels ne mesurent pas les canaux auxiliaires matériels.
Monocypher 4.0.3 contient le [correctif temporel amont](https://monocypher.org/changelog).
Provenance et sommes de contrôle figurent dans
`third_party/monocypher/UPSTREAM.json` ; les notices accompagnent la distribution.
