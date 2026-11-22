import { NgModule } from '@angular/core';
import { NbCardModule, NbTooltipModule, NbCheckboxModule, NbSelectModule, NbIconModule, NbButtonModule, NbBadgeModule, NbInputModule } from '@nebular/theme';
import { SettingsComponent } from './settings.component';
import { ProfilesComponent } from './profiles/profiles.component';
import { NbThemeModule, NbLayoutModule } from '@nebular/theme';
import { CommonModule } from '@angular/common';
import { EditModule } from '../edit/edit.module';
import { NbSpinnerModule } from '@nebular/theme';
import { NbProgressBarModule } from '@nebular/theme';
import { I18nModule } from '../../@i18n/i18n.module';
import { FormsModule, ReactiveFormsModule } from '@angular/forms';

@NgModule({
  declarations: [SettingsComponent, ProfilesComponent],
  imports: [
    CommonModule,
    NbCardModule,
    NbLayoutModule,
    NbThemeModule,
    NbIconModule,
    NbButtonModule,
    NbInputModule,
    EditModule,
    NbSpinnerModule,
    NbProgressBarModule,
    NbBadgeModule,
    I18nModule,
    NbSelectModule,
    NbCheckboxModule,
    FormsModule,
    ReactiveFormsModule,
    NbTooltipModule,
  ]
})
export class SettingsModule { }
