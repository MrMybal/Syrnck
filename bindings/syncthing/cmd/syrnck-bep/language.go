package main

import "strings"

// Protocol JSON and identifiers stay canonical. Translate CLI-owned human
// diagnostics while preserving operating-system and library error details.
func humanError(language string, err error) string {
	value := err.Error()
	if language != "fr" {
		return value
	}
	messages := map[string]string{
		"syrnck BEP: encrypted receiver requires paired peers":                  "syrnck BEP : le récepteur chiffré exige des pairs associés",
		"syrnck BEP: encrypted receiver Store required":                         "syrnck BEP : un Store de récepteur chiffré est requis",
		"syrnck BEP: encrypted receiver closed":                                 "syrnck BEP : le récepteur chiffré est fermé",
		"syrnck BEP: receiver local certificate identity differs":               "syrnck BEP : l’identité du certificat local du récepteur diffère",
		"syrnck BEP: receiver connection identity differs":                      "syrnck BEP : l’identité de connexion du récepteur diffère",
		"syrnck BEP: encrypted receiver requires a running trusted peer folder": "syrnck BEP : le récepteur chiffré exige un dossier actif chez un pair de confiance",
		"syrnck BEP: invalid encrypted receiver agreement":                      "syrnck BEP : accord de récepteur chiffré invalide",
		"syrnck BEP: invalid encrypted file metadata":                           "syrnck BEP : métadonnées de fichier chiffré invalides",
		"syrnck BEP: invalid encrypted file type":                               "syrnck BEP : type de fichier chiffré invalide",
		"syrnck BEP: invalid encrypted metadata-only entry":                     "syrnck BEP : entrée de métadonnées chiffrées sans contenu invalide",
		"syrnck BEP: invalid encrypted block layout":                            "syrnck BEP : agencement de blocs chiffrés invalide",
		"syrnck BEP: invalid encrypted data binding":                            "syrnck BEP : liaison de données chiffrées invalide",
		"syrnck BEP: invalid encrypted publication origin":                      "syrnck BEP : origine de publication chiffrée invalide",
		"syrnck BEP: encrypted block manifest size differs":                     "syrnck BEP : la taille du manifeste de bloc chiffré diffère",
		"syrnck BEP: invalid encrypted folder token":                            "syrnck BEP : preuve de dossier chiffré invalide",
		"syrnck BEP: encrypted folder token missing":                            "syrnck BEP : la preuve de dossier chiffré est absente",
		"syrnck BEP: encrypted receiver password token changed":                 "syrnck BEP : la preuve de mot de passe du récepteur chiffré a changé",
		"syrnck BEP: invalid encrypted block receipt":                           "syrnck BEP : reçu de bloc chiffré invalide",
		"syrnck BEP: encrypted block receipt binding differs":                   "syrnck BEP : la liaison du reçu de bloc chiffré diffère",
		"syrnck BEP: encrypted receipt part size differs":                       "syrnck BEP : la taille d’une partie du reçu chiffré diffère",
		"syrnck BEP: encrypted block length mismatch":                           "syrnck BEP : la longueur du bloc chiffré diffère",

		"receive_encrypted requires an explicit initialized storage directory":                           "receive_encrypted requiert un dossier storage explicite et initialisé",
		"receive_encrypted cannot use workspace, directory or plaintext working-file options":            "receive_encrypted ne peut pas utiliser workspace, directory ou les options de fichiers de travail en clair",
		"receive_encrypted cannot use a folder password, untrusted_peer or revision author/signing keys": "receive_encrypted ne peut pas utiliser de mot de passe de dossier, untrusted_peer ou les options d’auteur et de signature des révisions",

		"folder_password_env requires untrusted_peer":                                              "folder_password_env requiert untrusted_peer",
		"invalid folder password environment variable name":                                        "nom de variable du mot de passe de dossier invalide",
		"folder password requires a separate environment variable":                                 "le mot de passe de dossier requiert une variable distincte",
		"folder password environment variable must contain 1..4096 UTF-8 bytes":                    "la variable du mot de passe de dossier doit contenir 1 à 4096 octets UTF-8",
		"syrnck BEP: folder password requires an untrusted peer":                                   "syrnck BEP : le mot de passe de dossier requiert un pair non fiable",
		"syrnck BEP: untrusted peer requires a UTF-8 folder password of 1..4096 bytes without NUL": "syrnck BEP : le pair non fiable exige un mot de passe de dossier de 1 à 4096 octets UTF-8 sans NUL",
		"syrnck BEP: encrypted folder session binding changed":                                     "syrnck BEP : le contexte de la session de dossier chiffrée a changé",
		"syrnck BEP: untrusted peer did not confirm folder encryption":                             "syrnck BEP : le pair non fiable n’a pas confirmé le chiffrement du dossier",
		"syrnck BEP: untrusted peer folder password token differs":                                 "syrnck BEP : la preuve du mot de passe de dossier du pair non fiable diffère",
		"syrnck BEP: invalid folder encryption checkpoint":                                         "syrnck BEP : point de contrôle du chiffrement de dossier invalide",
		"syrnck BEP: folder encryption checkpoint refuses downgrade or changed password":           "syrnck BEP : le point de contrôle refuse le passage en clair ou le changement de mot de passe",
		"choose signing_key_env or signing_key_file":                                               "choisir signing_key_env ou signing_key_file",
		"signing_key_password_env requires signing_key_file":                                       "signing_key_password_env requiert signing_key_file",
		"signing_key_file requires an explicit author":                                             "signing_key_file requiert un auteur explicite",
		"invalid signing key password environment variable name":                                   "nom de variable du mot de passe de signature invalide",
		"signing key password requires a separate environment variable":                            "le mot de passe de signature requiert une variable distincte",
		"signing key password environment variable must contain 1..4096 bytes":                     "la variable du mot de passe de signature doit contenir 1 à 4096 octets",
		"signing_key_file must be one regular non-linked 104-byte envelope":                        "signing_key_file doit être une enveloppe de 104 octets dans un fichier ordinaire sans lien",
		"signing_key_file changed while opening":                                                   "signing_key_file a changé pendant son ouverture",
		"protected signing key must be outside the workspace or inside its protected Store":        "la clé de signature protégée doit être hors du workspace ou dans son Store protégé",

		"key_password_env requires key_file":                                                   "key_password_env requiert key_file",
		"invalid key password environment variable name":                                       "nom de variable d’environnement de mot de passe invalide",
		"key password requires a separate environment variable":                                "le mot de passe de clé demande une variable d’environnement distincte",
		"key_file conflicts with a nonempty raw storage key environment variable":              "key_file est incompatible avec une variable de clé de stockage brute non vide",
		"key password environment variable must contain 1..4096 bytes":                         "la variable de mot de passe de clé doit contenir 1..4096 octets",
		"key_file must be one regular non-linked 104-byte envelope":                            "key_file doit être une enveloppe de 104 octets dans un fichier ordinaire sans lien",
		"protected storage key must be outside the workspace or inside its protected Store":    "la clé de stockage protégée doit être hors du workspace ou dans son Store protégé",
		"key_file changed while opening":                                                       "key_file a changé pendant son ouverture",
		"input file exceeds size limit":                                                        "le fichier dépasse la taille autorisée",
		"configuration nesting exceeds limit":                                                  "la configuration est trop imbriquée",
		"configuration contains duplicate or invalid members":                                  "la configuration contient des champs dupliqués ou invalides",
		"invalid configuration structure":                                                      "structure de configuration invalide",
		"configuration must be UTF-8":                                                          "la configuration doit être en UTF-8",
		"configuration must contain one JSON object":                                           "la configuration doit contenir un seul objet JSON",
		"configuration version 1, directory, folder, certificate and private_key are required": "version 1 et champs directory, folder, certificate et private_key requis",
		"mode must be dial or listen":                                                          "mode doit valoir dial ou listen",
		"poll_ms must be 50..60000 or zero for default":                                        "poll_ms doit valoir 50..60000, ou zéro pour la valeur par défaut",
		"retry bounds must be in 50..60000 milliseconds":                                       "les bornes de reconnexion doivent être comprises entre 50 et 60000 millisecondes",
		"address must be host:port":                                                            "address doit être de la forme hôte:port",
		"invalid TCP address":                                                                  "adresse TCP invalide",
		"invalid key environment variable name":                                                "nom de variable d’environnement de clé invalide",
		"one PEM device certificate is required":                                               "un unique certificat PEM d’appareil est requis",
		"invalid device certificate":                                                           "certificat d’appareil invalide",
		"private device key must be outside the workspace or inside its protected Store":       "la clé privée d’appareil doit être hors du workspace ou dans son Store protégé",
		"cannot load matching device certificate and key":                                      "impossible de charger le certificat d’appareil et sa clé correspondante",
		"one leaf device certificate is required":                                              "un unique certificat terminal d’appareil est requis",
		"an explicit different paired peer ID is required":                                     "un Device ID de pair explicite et différent est requis",
		"storage key environment variable must contain 64 hexadecimal characters":              "la variable de clé de stockage doit contenir 64 caractères hexadécimaux",
		"author requires name, email and a timezone offset in -1439..1439 minutes":             "author requiert name, email et un décalage de fuseau entre -1439 et 1439 minutes",
		"signing_key_env requires an explicit author":                                          "signing_key_env requiert un auteur explicite",
		"storage and revision signing keys require different environment variables":            "les clés de stockage et de signature des révisions demandent des variables d’environnement distinctes",
		"invalid signing key environment variable name":                                        "nom de variable d’environnement de clé de signature invalide",
		"revision signing key environment variable must contain 64 hexadecimal characters":     "la variable de clé de signature des révisions doit contenir 64 caractères hexadécimaux",
		"resolved filesystem path exceeds limit":                                               "le chemin résolu dépasse la limite autorisée",
		"unsupported resolved filesystem path":                                                 "chemin résolu non pris en charge",
		"--lang must be en or fr":                                                              "--lang doit valoir en ou fr",
		"expected run or device-id":                                                            "une commande run ou device-id est attendue",
		"a configuration or certificate file is required":                                      "un fichier de configuration ou de certificat est requis",
	}
	if translated, ok := messages[value]; ok {
		return translated
	}
	for english, french := range map[string]string{
		"syrnck: native signing key is required for new revisions": "syrnck: la clé de signature native est requise pour les nouvelles révisions",
		"resolve workspace directory: ":                            "résolution du dossier de travail : ",
		"resolve Store directory: ":                                "résolution du Store : ",
		"resolve private device key: ":                             "résolution de la clé privée d’appareil : ",
		"json: unknown field ":                                     "champ JSON inconnu : ",
		"flag provided but not defined: ":                          "option inconnue : ",
		"flag needs an argument: ":                                 "argument requis pour l’option : ",
	} {
		if suffix, ok := strings.CutPrefix(value, english); ok {
			return french + suffix
		}
	}
	return value
}
