terraform {
  backend "gcs" {
    bucket = "pixel-project-state"
    prefix = "crux/projects/pixel-portal/terraform"
  }
}

provider "google" {
  project = local.project_id
  region  = local.region
}

locals {
  project_id = "pixel-project"
  region     = "europe-west1"
}

resource "google_artifact_registry_repository" "pixel_portal_repo" {
  location      = local.region
  repository_id = "pixel-portal-repo"
  format        = "DOCKER"
}

resource "google_cloud_run_service" "pixel_portal" {
  name     = "pixel-portal"
  location = local.region

  template {
    spec {
      containers {
        image = "europe-west1-docker.pkg.dev/${local.project_id}/pixel-portal-repo/pixel-portal:v0.0.1-manual3"
        ports {
          container_port = 8080
        }
      }
    }
  }

  traffic {
    percent         = 100
    latest_revision = true
  }
}

# Allow the Cloud Run service to be accessed publicly
resource "google_cloud_run_service_iam_member" "pixel_portal_public" {
  service  = google_cloud_run_service.pixel_portal.name
  location = google_cloud_run_service.pixel_portal.location
  role     = "roles/run.invoker"
  member   = "allUsers"
}

output "cloud_run_url" {
  value = google_cloud_run_service.pixel_portal.status[0].url
}
