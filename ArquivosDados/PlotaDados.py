import numpy as np
import matplotlib.pyplot as plt
import csv

# Lendo os dados do arquivo CSV
with open('mpu_data4.csv', 'r') as file:
    reader = csv.reader(file)
    header = next(reader)  # Lê o cabeçalho
    data = list(reader)

# Convertendo para array numpy
data = np.array(data, dtype=float)

# Extraindo colunas
num_amostra = data[:, 0]
accel_x = data[:, 1]
accel_y = data[:, 2]
accel_z = data[:, 3]
giro_x = data[:, 4]
giro_y = data[:, 5]
giro_z = data[:, 6]

# Criando figura com subplots
plt.figure(figsize=(12, 8))

# Plotando dados do acelerômetro
plt.subplot(2, 1, 1)
plt.plot(num_amostra, accel_x, 'r-', label='Accel X')
plt.plot(num_amostra, accel_y, 'g-', label='Accel Y')
plt.plot(num_amostra, accel_z, 'b-', label='Accel Z')
plt.title("Dados do Acelerômetro e Giroscópio")
plt.ylabel("Acelerômetro")
plt.grid()
plt.legend()

# Plotando dados do giroscópio
plt.subplot(2, 1, 2)
plt.plot(num_amostra, giro_x, 'r-', label='Giro X')
plt.plot(num_amostra, giro_y, 'g-', label='Giro Y')
plt.plot(num_amostra, giro_z, 'b-', label='Giro Z')
plt.xlabel("Tempo (amostras)")
plt.ylabel("Giroscópio")
plt.grid()
plt.legend()

plt.tight_layout()
plt.show()